"""Conditional onset/window relocation with actual detector, recorder and DSP C.

Only saved samples may enter DSP. The complete A window and every B j/j+1 read
across the +/-127 search are checked before evaluation. Unknown positions outside
those reads contain two distinct canaries; equal DSP results prove independence,
not reconstruction of those samples. Full capture flags, stream continuity,
prehistory/filter state and the PTP fit are not revalidated. Results are local
conditional comparisons, never a hardware success rate or reconstructed capture.
"""
from __future__ import annotations
import argparse
import ctypes as ct
import hashlib
import json
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
import test_range_diagnostics as core
import test_algorithm_replay as corpus
sys.path.insert(0, str(core.ROOT / "App/AudioArrival"))
import test_arrival_detector as detector


HELPER = r'''
#include "arrival_detector.h"
#include "range_recorder.h"
#include <stddef.h>
#include <limits.h>
size_t detector_size(void) { return sizeof(ArrivalDetector); }
size_t detection_size(void) { return sizeof(ArrivalDetection); }
size_t onset_offset(void) { return offsetof(ArrivalDetector, onset); }
size_t confirmation_offset(void) { return offsetof(ArrivalDetection, confirmation_samples); }
bool recorder_window(unsigned sample, unsigned fraction, unsigned confirmed,
                     uint64_t onset_ns, const RangeCapture *source,
                     uint64_t *start_ns, int *shift) {
    const uint64_t base = 1000000ULL;
    RangeRecorder r = {0};
    r.available = RANGE_HISTORY_SAMPLES; r.next_sample = base + confirmed + 1U;
    if (!RangeRecorder_Arm(&r, source->sequence, source->generation, base + sample,
                           (uint16_t)fraction, onset_ns, source->period_ps, source->flags)) return false;
    *start_ns = r.start_ns;
    *shift = (int)((int64_t)r.first_sample - (int64_t)base);
    return true;
}
/* All reads are checked, using the same float expressions/casts as real DSP. */
bool inspect_reads(const RangeCapture *a, const RangeCapture *b, int shift_a, int shift_b,
                   unsigned begin, unsigned end, int bounds[4], uint32_t *count) {
    float ratio = (float)a->period_ps / (float)b->period_ps;
    bounds[0] = bounds[2] = INT_MAX; bounds[1] = bounds[3] = INT_MIN;
    *count = 0; bool valid = true;
    for (unsigned i = begin; i < end; ++i) {
        int source_a = (int)i + shift_a;
        if (source_a < bounds[0]) bounds[0] = source_a;
        if (source_a > bounds[1]) bounds[1] = source_a;
        if (source_a < 0 || source_a >= (int)RANGE_SAMPLES) valid = false;
        for (int k = -RANGE_SEARCH_SAMPLES; k <= RANGE_SEARCH_SAMPLES; ++k) {
            *count += 1U;
            float position = ((float)i + (float)k) * ratio;
            if (position < 0.0f || position >= (float)(RANGE_SAMPLES-1U)) { valid = false; continue; }
            unsigned j = (unsigned)position;
            int source_b = (int)j + shift_b;
            if (source_b < bounds[2]) bounds[2] = source_b;
            if (source_b + 1 > bounds[3]) bounds[3] = source_b + 1;
            if (source_b < 0 || source_b + 1 >= (int)RANGE_SAMPLES) valid = false;
        }
    }
    return valid;
}
'''


def build_helpers(cc, run):
    helper=run/'onset_helpers.c'; helper.write_text(HELPER,encoding='ascii')
    output=run/'onset_helpers.dll'
    subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-shared',
        '-I'+str(core.ROOT/'App/AudioArrival'),'-I'+str(core.ROOT/'App/Ranging'),
        str(helper),str(core.ROOT/'App/AudioArrival/arrival_detector.c'),
        str(core.ROOT/'App/Ranging/range_recorder.c'),'-o',str(output)],check=True)
    dll=ct.CDLL(str(output))
    for name in ('detector_size','detection_size','onset_offset','confirmation_offset'):
        getattr(dll,name).restype=ct.c_size_t
    assert dll.detector_size()==ct.sizeof(detector.Detector)
    assert dll.detection_size()==ct.sizeof(detector.Detection)
    assert dll.onset_offset()==detector.Detector.onset.offset
    assert dll.confirmation_offset()==detector.Detection.confirmation_samples.offset
    dll.ArrivalDetector_Reset.argtypes=[ct.POINTER(detector.Detector)]
    dll.ArrivalDetector_Push.argtypes=[ct.POINTER(detector.Detector),ct.c_int16,ct.c_uint64,ct.POINTER(detector.Detection)]
    dll.ArrivalDetector_Push.restype=ct.c_bool
    dll.recorder_window.argtypes=[ct.c_uint,ct.c_uint,ct.c_uint,ct.c_uint64,ct.POINTER(core.Capture),ct.POINTER(ct.c_uint64),ct.POINTER(ct.c_int)]
    dll.recorder_window.restype=ct.c_bool
    dll.inspect_reads.argtypes=[ct.POINTER(core.Capture),ct.POINTER(core.Capture),ct.c_int,ct.c_int,
                                ct.c_uint,ct.c_uint,ct.POINTER(ct.c_int),ct.POINTER(ct.c_uint32)]
    dll.inspect_reads.restype=ct.c_bool
    return dll


def noise_events(folder):
    result={}
    for port in ('COM3','COM8'):
        values=[]
        for match in re.finditer(r'AUDIO EVENT seq=\d+ arrival=(\d+)\.(\d+)[^\r\n]*?noise=(\d+)',
                                 (folder/(port+'.log')).read_text(encoding='utf-8-sig')):
            values.append((int(match[1])*10**9+int(match[2]),int(match[3])))
        result[port]=values
    return result


def new_event(dll, pcm, noise):
    d=detector.Detector();dll.ArrivalDetector_Reset(ct.byref(d));d.warmup=0;d.noise_q8=noise*256
    for sample,value in enumerate(pcm):
        event=detector.Detection()
        if dll.ArrivalDetector_Push(ct.byref(d),value,sample,ct.byref(event)):
            return dict(sample=int(event.sample),fraction_q16=int(event.fraction_q16),confirmed_at=sample,
                        confirmation_samples=int(event.confirmation_samples),peak=int(event.peak),
                        threshold=int(event.threshold),noise=int(event.noise))
    return None


def relocated_capture(dll, clip, events, canary):
    filename,metadata,old=clip
    possible=events[metadata['port']]
    nearest=min(possible,key=lambda v:abs(v[0]-old.onset_ns)) if possible else None
    noise=nearest[1] if nearest and abs(nearest[0]-old.onset_ns)<=20_000_000 else 4
    event=new_event(dll,old.pcm,noise)
    if event is None:return None,dict(file=filename,initial_noise=noise,reason='no local detector acceptance')
    # Same saved linear clock model. Use integer Q16 throughout; this does not
    # re-fit unavailable DMA/PTP anchors or claim nanosecond-identical board time.
    onset_ns=old.start_ns+((event['sample']*65536+event['fraction_q16'])*old.period_ps)//(65536*1000)
    start=ct.c_uint64();shift=ct.c_int()
    ok=dll.recorder_window(event['sample'],event['fraction_q16'],event['confirmed_at'],onset_ns,
                           ct.byref(old),ct.byref(start),ct.byref(shift))
    assert ok
    before=((256*65536+event['fraction_q16'])*old.period_ps)//(65536*1000)
    assert start.value==onset_ns-before and shift.value==event['sample']-256
    updated=core.Capture.from_buffer_copy(bytes(old))
    updated.onset_ns=onset_ns;updated.start_ns=start.value
    updated.pcm[:]=[old.pcm[i+shift.value] if 0<=i+shift.value<2048 else canary for i in range(2048)]
    detail=dict(file=filename,initial_noise=noise,event=event,shift_samples=shift.value,
                old_start_ns=old.start_ns,new_start_ns=start.value,new_onset_ns=onset_ns,
                missing_prefix=max(0,-shift.value),missing_suffix=max(0,shift.value),
                full_capture_reconstructed=shift.value==0,
                condition='original flags retained conditionally; full stream, missing samples and recorder Copy clipping check were not revalidated')
    return updated,detail


def domain(dll,a,b,da,db,begin,end):
    bounds=(ct.c_int*4)();count=ct.c_uint32()
    valid=dll.inspect_reads(ct.byref(a),ct.byref(b),da,db,begin,end,bounds,ct.byref(count))
    assert count.value==(end-begin)*255
    return dict(available=bool(valid),original_a_indices=[bounds[0],bounds[1]],
                original_b_indices=[bounds[2],bounds[3]],checked_interpolation_positions=count.value,
                reason=None if valid else 'required DSP samples extend outside the saved clip; no DSP evaluation performed')


def aggregate(rows,key):
    selected=[row[key] for row in rows if key in row]
    passed=[r for r in selected if r['ok']]
    return dict(eligible_comparisons=len(selected),conditional_passed=len(passed),
                errors=dict(__import__('collections').Counter(r['error'] for r in selected)),
                conditional_distance_mm=corpus.metrics([r['distance_mm'] for r in passed]))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--captures-root',type=Path,default=core.ROOT/'build/pcm-capture')
    parser.add_argument('--baseline-ref',default='3363294')
    args=parser.parse_args()
    parent=core.ROOT/'build/onset-replay-validation';parent.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
    current,before,_=core.build(args.cc,run,args.baseline_ref)
    helper=build_helpers(args.cc,run)
    summaries={}
    for name,expect in corpus.GROUPS.items():
        folder=args.captures_root/name
        if not folder.is_dir():print('SKIP unavailable fixture',folder);continue
        boards,pairs,extras=corpus.load_group(folder);events=noise_events(folder);rows=[]
        for ca,cb in pairs:
            row=dict(a_seq=ca[2].sequence,b_seq=cb[2].sequence,
                     fixed_96=corpus.result_dict(*core.call(before,ca[2],cb[2])),
                     fixed_127=corpus.result_dict(*core.call(current,ca[2],cb[2])))
            a,da=relocated_capture(helper,ca,events,-32768)
            b,db=relocated_capture(helper,cb,events,-32768)
            row['A']=da;row['B']=db
            if a is None or b is None:
                row['skip_reason']='local detector did not accept one side';rows.append(row);continue
            for window,limits in [('LONG',(128,1920)),('FRONT',(128,384))]:
                check=domain(helper,a,b,da['shift_samples'],db['shift_samples'],*limits)
                row[window+'_domain']=check
                if not check['available']:continue
                first=core.call(current,a,b,window=limits)
                a2,da2=relocated_capture(helper,ca,events,32767)
                b2,db2=relocated_capture(helper,cb,events,32767)
                assert da2==da and db2==db
                second=core.call(current,a2,b2,window=limits)
                assert first[0]==second[0] and bytes(first[1])==bytes(second[1]),'unread canary affected DSP'
                row['relocated_'+window]=corpus.result_dict(*first)
                row['relocated_'+window]['unread_canary_independence']=True
            rows.append(row)
        summary=dict(label=expect['label'],saved_capture_counts={b:len(v) for b,v in boards.items()},paired_count=len(pairs),extras=extras,
                     fixed_96=aggregate(rows,'fixed_96'),fixed_127=aggregate(rows,'fixed_127'),
                     relocated_LONG=aggregate(rows,'relocated_LONG'),relocated_FRONT=aggregate(rows,'relocated_FRONT'),
                     skipped_LONG=[dict(a_seq=r['a_seq'],b_seq=r['b_seq'],A=r['A'],B=r['B'],domain=r.get('LONG_domain')) for r in rows if 'relocated_LONG' not in r])
        summaries[name]=summary
        (run/(name+'.json')).write_text(json.dumps(dict(summary=summary,rows=rows),indent=2),encoding='utf-8')
        print('CONDITIONAL:',name,'fixed96',summary['fixed_96']['conditional_passed'],
              'fixed127',summary['fixed_127']['conditional_passed'],'relocated LONG',summary['relocated_LONG'],
              'skipped',len(summary['skipped_LONG']))
    report=dict(scope=__doc__,clock_policy='Integer Q16 on saved period/start model; recorder start formula calls real RangeRecorder_Arm.',
                canary_policy='Unknown unused positions contain -32768 then +32767; evaluated outputs must be identical. These values are not invented acoustic samples.',
                source_sha256={str(path.relative_to(core.ROOT)):hashlib.sha256(path.read_bytes()).hexdigest() for path in
                    [core.ROOT/'App/AudioArrival/arrival_detector.c',core.ROOT/'App/AudioArrival/arrival_detector.h',
                     core.ROOT/'App/Ranging/range_core.c',core.ROOT/'App/Ranging/range_core.h',core.ROOT/'App/Ranging/range_recorder.c']},
                datasets=summaries)
    (run/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('Conditional comparisons completed; not reconstructed captures or board success rates. Artifacts:',run)


if __name__=='__main__':main()
