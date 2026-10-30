param(
    [string[]]$Ports = @('COM3', 'COM8'),
    [int]$BaudRate = 1152000,
    [int]$DurationSeconds = 90,
    [string]$OutputDirectory = '',
    # Offline recovery of PCM frames from saved MobaXterm/raw serial logs.
    [string[]]$LogFiles = @()
)
$ErrorActionPreference = 'Stop'
if ($DurationSeconds -le 0) { throw 'DurationSeconds must be positive.' }
if (!$OutputDirectory) {
    $root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
    $OutputDirectory = Join-Path $root ('build\pcm-capture\' + (Get-Date -Format 'yyyyMMdd_HHmmss_fff'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

if (-not ('PcmCaptureCodec' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
public static class PcmCaptureCodec {
    public static byte[] Hex(string text) {
        if (text.Length % 2 != 0) throw new FormatException("Odd hex length");
        byte[] data = new byte[text.Length / 2];
        for (int i = 0; i < data.Length; i++) data[i] = Convert.ToByte(text.Substring(i * 2, 2), 16);
        return data;
    }
    public static uint Crc32(byte[] data) {
        uint crc = 0xffffffffu;
        foreach (byte b in data) {
            crc ^= b;
            for (int i = 0; i < 8; i++) crc = (crc >> 1) ^ ((crc & 1) != 0 ? 0xedb88320u : 0u);
        }
        return crc ^ 0xffffffffu;
    }
}
'@
}

function New-CaptureState([string]$PortName) {
    return @{ Port = $PortName; Pending = ''; Active = $null; Count = 0; Saved = 0; Bad = 0; Serial = $null; Writer = $null }
}
function Get-Fields([string]$Line) {
    $fields = @{}
    foreach ($match in [regex]::Matches($Line, '(\w+)=([^\s]+)')) { $fields[$match.Groups[1].Value] = $match.Groups[2].Value }
    return $fields
}
function Write-Wave([string]$Path, [byte[]]$Data, [int]$SampleRate) {
    $writer = [IO.BinaryWriter]::new([IO.File]::Create($Path))
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'))
        $writer.Write([uint32](36 + $Data.Length))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '))
        $writer.Write([uint32]16); $writer.Write([uint16]1); $writer.Write([uint16]1)
        $writer.Write([uint32]$SampleRate); $writer.Write([uint32]($SampleRate * 2))
        $writer.Write([uint16]2); $writer.Write([uint16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data')); $writer.Write([uint32]$Data.Length)
        $writer.Write($Data)
    } finally { $writer.Dispose() }
}
function Process-Line($State, [string]$Line) {
    $Line = $Line.TrimEnd("`r")
    if ($Line.StartsWith('BOOT ')) {
        if ($State.Active) { $State.Bad++; Write-Warning "$($State.Port): reset during PCM export" }
        $State.Active = $null
    }
    if (!$Line.StartsWith('PCM ')) {
        if ($Line -match '^(BOOT|SYNC|AUDIO EVENT|AUDIO WARN|AUDIO ERROR|AUDIO RECOVER|RANGE RESULT|RANGE REJECT|RANGE CMP|RANGE DSP)') {
            Write-Host "[$($State.Port)] $Line"
        }
        return
    }
    try {
        $fields = Get-Fields $Line
        if ($Line.StartsWith('PCM BEGIN ')) {
            if ($State.Active) { $State.Bad++; Write-Warning "$($State.Port): incomplete previous PCM frame" }
            $State.Active = $null
            foreach ($key in @('v','id','board','seq','epoch','count','fs','start_ns','onset_ns','period_ps','flags')) {
                if (!$fields.ContainsKey($key)) { throw "Missing header $key" }
            }
            if ($fields.v -ne '1' -or [int]$fields.count -ne 2048 -or [int]$fields.fs -ne 48000) { throw 'Unsupported PCM format' }
            if ($fields.board -notmatch '^[AB?]$') { throw 'Invalid board label' }
            $null = [uint32]$fields.id; $null = [uint32]$fields.seq; $null = [uint32]$fields.epoch
            $null = [uint64]$fields.start_ns; $null = [uint64]$fields.onset_ns; $null = [uint64]$fields.period_ps
            $null = [uint32]$fields.flags
            $State.Count++
            $State.Active = @{ Meta = $fields; Data = [byte[]]::new(4096); Samples = 0; Index = $State.Count }
        } elseif ($Line.StartsWith('PCM DATA ')) {
            foreach ($key in @('id','offset','hex')) {
                if (!$fields.ContainsKey($key)) { throw "Missing DATA field $key" }
            }
            $frame = $State.Active
            if (!$frame -or $fields.id -ne $frame.Meta.id) { throw 'DATA has no matching BEGIN' }
            if ([int]$fields.offset -ne $frame.Samples) { throw 'Missing, duplicated or reordered PCM block' }
            if (!$fields.hex -or $fields.hex -notmatch '^[0-9a-fA-F]+$' -or $fields.hex.Length % 4) { throw 'Invalid PCM hex' }
            [byte[]]$bytes = [PcmCaptureCodec]::Hex($fields.hex)
            if ($bytes.Length -gt 128 -or $frame.Samples * 2 + $bytes.Length -gt $frame.Data.Length) { throw 'PCM block out of bounds' }
            [Array]::Copy($bytes, 0, $frame.Data, $frame.Samples * 2, $bytes.Length)
            $frame.Samples += $bytes.Length / 2
        } elseif ($Line.StartsWith('PCM END ')) {
            $frame = $State.Active
            if (!$frame -or $fields.id -ne $frame.Meta.id -or $frame.Samples -ne 2048) { throw 'Incomplete PCM frame' }
            if (!$fields.crc32 -or $fields.crc32 -notmatch '^[0-9a-fA-F]{8}$') { throw 'Missing CRC32' }
            $crc = [PcmCaptureCodec]::Crc32($frame.Data).ToString('x8')
            if ($crc -ne $fields.crc32) { throw "CRC32 mismatch: expected $($fields.crc32), got $crc" }
            $stem = '{0}_{1:D4}_{2}_seq{3}' -f $State.Port, [int]$frame.Index, $frame.Meta.board, $frame.Meta.seq
            $base = Join-Path $OutputDirectory $stem
            [IO.File]::WriteAllBytes(($base + '.pcm'), $frame.Data)
            Write-Wave ($base + '.wav') $frame.Data 48000
            $meta = [ordered]@{
                port = $State.Port; capture_index = $frame.Index; board = $frame.Meta.board
                dump_id = $frame.Meta.id; sequence = $frame.Meta.seq; epoch = $frame.Meta.epoch
                samples = 2048; encoding = 's16le'; wav_sample_rate = 48000
                start_ns = $frame.Meta.start_ns; onset_ns = $frame.Meta.onset_ns
                period_ps = $frame.Meta.period_ps; flags = $frame.Meta.flags; crc32 = $crc
            }
            [IO.File]::WriteAllText(($base + '.json'), ($meta | ConvertTo-Json), [Text.UTF8Encoding]::new($false))
            $State.Saved++; $State.Active = $null
            Write-Host "[$($State.Port)] PCM saved: $stem (2048 samples, CRC OK)"
        } elseif ($Line.StartsWith('PCM ABORT ')) {
            throw 'Board aborted UART export'
        } elseif ($Line.StartsWith('PCM WARN ')) {
            Write-Warning "[$($State.Port)] $Line"
        }
    } catch {
        $State.Bad++; $State.Active = $null
        Write-Warning "[$($State.Port)] $($_.Exception.Message)"
    }
}

$states = @()
try {
    if ($LogFiles.Count) {
        foreach ($path in $LogFiles) {
            $name = [IO.Path]::GetFileNameWithoutExtension($path) -replace '[^A-Za-z0-9_-]', '_'
            $state = New-CaptureState $name; $states += $state
            foreach ($line in [IO.File]::ReadLines([IO.Path]::GetFullPath($path))) { Process-Line $state $line }
        }
    } else {
        $Ports = @($Ports | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim().ToUpperInvariant() } | Select-Object -Unique)
        foreach ($portName in $Ports) {
            if ($portName -notmatch '^COM\d+$') { throw "Invalid serial port: $portName" }
            $state = New-CaptureState $portName; $states += $state
            $serial = [IO.Ports.SerialPort]::new($portName, $BaudRate, [IO.Ports.Parity]::None, 8, [IO.Ports.StopBits]::One)
            $state.Serial = $serial
            $serial.Handshake = [IO.Ports.Handshake]::None
            $serial.DtrEnable = $false; $serial.RtsEnable = $false
            $serial.ReadBufferSize = 65536; $serial.ReadTimeout = 100
            $state.Writer = [IO.StreamWriter]::new((Join-Path $OutputDirectory ($portName + '.log')), $false, [Text.UTF8Encoding]::new($false))
            $state.Writer.AutoFlush = $true
            $serial.Open()
        }
        # Discard bytes retained by the driver from a previous capture. All
        # ports are open before this boundary; play only after READY below.
        foreach ($state in $states) { $state.Serial.DiscardInBuffer() }
        Write-Host "READY: $($Ports -join ', ') at $BaudRate baud. Play the test audio now."
        Write-Host "Capturing for $DurationSeconds seconds. Output: $OutputDirectory"
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while ($timer.Elapsed.TotalSeconds -lt $DurationSeconds) {
            foreach ($state in $states) {
                $text = $state.Serial.ReadExisting()
                if (!$text) { continue }
                $state.Writer.Write($text)
                $state.Pending += $text
                while (($newline = $state.Pending.IndexOf("`n")) -ge 0) {
                    $line = $state.Pending.Substring(0, $newline)
                    $state.Pending = $state.Pending.Substring($newline + 1)
                    Process-Line $state $line
                }
                if ($state.Pending.Length -gt 8192) {
                    $state.Bad++; $state.Active = $null; $state.Pending = ''
                    Write-Warning "$($state.Port): overlong/unterminated serial line"
                }
            }
            Start-Sleep -Milliseconds 5
        }
    }
} finally {
    foreach ($state in $states) {
        if ($state.Active) { $state.Bad++; Write-Warning "$($state.Port): incomplete PCM frame at end" }
        if ($state.Serial) { $state.Serial.Dispose() }
        if ($state.Writer) { $state.Writer.Dispose() }
        Write-Host "$($state.Port): saved=$($state.Saved) incomplete/invalid=$($state.Bad)"
    }
    Write-Host "Files: $OutputDirectory"
}
