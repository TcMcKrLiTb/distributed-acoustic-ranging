"""End-to-end firmware encoder -> PowerShell decoder checks; no serial hardware.

Run with Python 3, a native C compiler (gcc) and Windows PowerShell available.
All generated fixtures go under build/pcm-validation.
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import wave
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--powershell", default="powershell.exe")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    work = root / "build" / "pcm-validation"
    work.mkdir(parents=True, exist_ok=True)
    # Each invocation uses a new folder; never deletes an existing test run.
    import tempfile
    run = Path(tempfile.mkdtemp(prefix="run-", dir=work))
    stubs = run / "stubs"
    stubs.mkdir()
    (stubs / "main.h").write_text("""
#include <stdint.h>
typedef struct { int unused; } UART_HandleTypeDef;
typedef enum { HAL_OK, HAL_ERROR } HAL_StatusTypeDef;
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *, uint8_t *, uint16_t, uint32_t);
""")
    (stubs / "FreeRTOS.h").write_text("#define taskENTER_CRITICAL() ((void)0)\n#define taskEXIT_CRITICAL() ((void)0)\n")
    (stubs / "task.h").write_text("")
    fixture = run / "fixture.c"
    fixture.write_text(r'''
#include "main.h"
#include "Diagnostics/pcm_dump.h"
#include "TimeSync/time_sync.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
UART_HandleTypeDef huart1;
static unsigned calls, fail_at;
bool TimeSync_GetStatus(TimeSyncStatus *s) {
    memset(s, 0, sizeof(*s)); s->role = TIME_SYNC_MASTER; s->locked = true; return true;
}
void SyncLog_Write(const char *text) { fputs(text, stdout); }
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *u, uint8_t *data, uint16_t size, uint32_t timeout) {
    (void)u; (void)timeout;
    if (++calls == fail_at) return HAL_ERROR;
    fwrite(data, 1, size, stdout); return HAL_OK;
}
static void fill(RangeCapture *c, unsigned seq) {
    memset(c, 0, sizeof(*c)); c->sequence = seq; c->generation = 7;
    c->flags = RANGE_CAPTURE_LOCKED; c->period_ps = 20837209ULL;
    c->start_ns = 9007199254740993ULL + seq;
    c->onset_ns = c->start_ns + 5334325ULL;
    for (unsigned i = 0; i < RANGE_SAMPLES; ++i)
        c->pcm[i] = (int16_t)((int)((seq * 17U + i * 31U) % 65536U) - 32768);
    c->pcm[0] = -32768; c->pcm[1] = 32767; c->pcm[2] = -1; c->pcm[3] = 0;
}
int main(int argc, char **argv) {
    RangeCapture c; PcmDump_Init();
    if (argc > 1 && !strcmp(argv[1], "fail")) fail_at = 3;
    fill(&c, 101); PcmDump_Submit(&c);
    fill(&c, 202); PcmDump_Submit(&c);
    fill(&c, 303); PcmDump_Submit(&c); // Both slots occupied: drop diagnostic copy.
    memset(&c, 0, sizeof(c)); // The export must own its sample copy.
    if (calls) return 2; // The producer must never transmit.
    for (unsigned i = 0; i < 90; ++i) {
        unsigned before = calls; PcmDump_Poll();
        if (calls - before > 1) return 3; // At most one UART block per poll.
    }
    return 0;
}
''')
    exe = run / "fixture.exe"
    cc = shutil.which(args.cc) or args.cc
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(stubs), "-I" + str(root / "App"), str(fixture),
                    str(root / "App/Diagnostics/pcm_dump.c"),
                    str(root / "App/TimeSync/sync_format.c"), "-o", str(exe)], check=True)
    normal = subprocess.check_output([str(exe)], text=True)
    assert "PCM WARN dropped=1" in normal
    # Check CRC independently of the PowerShell implementation.
    data = bytearray()
    for line in normal.splitlines():
        if line.startswith("PCM BEGIN "):
            data.clear()
        elif line.startswith("PCM DATA "):
            data.extend(bytes.fromhex(line.split("hex=", 1)[1]))
        elif line.startswith("PCM END "):
            assert f"{zlib.crc32(data):08x}" == line.split("crc32=", 1)[1]

    def decode(name, text, expected_sequences, expected_console=()):
        source = run / (name + ".log")
        source.write_text(text, encoding="ascii")
        out = run / name
        result = subprocess.run([args.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass",
                                 "-File", str(root / "App/Diagnostics/capture_pcm.ps1"),
                                 "-LogFiles", str(source), "-OutputDirectory", str(out)],
                                capture_output=True, text=True, errors="replace")
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        for line in expected_console:
            assert line in result.stdout, result.stdout
        records = [json.loads(p.read_text(encoding="utf-8-sig")) for p in sorted(out.glob("*.json"))]
        assert [int(r["sequence"]) for r in records] == expected_sequences, result.stdout
        for meta, path in zip(records, sorted(out.glob("*.pcm"))):
            seq = int(meta["sequence"])
            samples = [((seq * 17 + i * 31) % 65536) - 32768 for i in range(2048)]
            samples[:4] = [-32768, 32767, -1, 0]
            expected = struct.pack("<2048h", *samples)
            assert path.read_bytes() == expected
            assert int(meta["start_ns"]) == 9007199254740993 + seq
            assert int(meta["period_ps"]) == 20837209
            with wave.open(str(path.with_suffix(".wav")), "rb") as wav:
                assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate(), wav.getnframes()) == (1, 2, 48000, 2048)
                assert wav.readframes(2048) == expected
        print("PASS:", name, "saved", len(records))

    decode("normal", normal, [101, 202])
    # Interleaved ordinary logs must not affect frame assembly.
    comparison = "RANGE CMP a=101 b=202 win=FRONT error=0 q=962 q2=930 gap=32 lag=11 frac=0 dt_ns=-2038000 mm=700 dsp_ms=20"
    setup = "RANGE DSP primary=LONG compare=FRONT long=128:1920 front=128:384"
    interleaved = normal.replace("PCM DATA ", "AUDIO EVENT seq=9\nPCM DATA ")
    interleaved = interleaved.replace("PCM DATA ", setup + "\n" + comparison + "\nPCM DATA ", 1)
    decode("interleaved", interleaved, [101, 202], [setup, comparison])
    corrupted = re.sub(r"(hex=)([0-9a-f])", lambda m: m[1] + ("1" if m[2] == "0" else "0"), normal, count=1)
    decode("corrupt", corrupted, [202])
    missing = re.sub(r"PCM DATA [^\n]+\n", "", normal, count=1)
    decode("missing", missing, [202])
    decode("truncated", normal.split("PCM END", 1)[0], [])
    aborted = subprocess.check_output([str(exe), "fail"], text=True)
    assert "PCM ABORT id=1" in aborted
    decode("uart_abort", aborted, [202])
    print("All encoder/decoder checks passed. Artifacts:", run)


if __name__ == "__main__":
    main()
