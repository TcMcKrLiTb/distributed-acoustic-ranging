"""Replay all archived PCM with snapshot +/-96 and current +/-127 C engines.

This is a fixed-capture search regression, not new acoustic validation. Detector
changes do not relocate these recorded windows. No missing samples are invented
or zero-filled. Per-event results and source hashes are retained under build/.
Uses only Python standard library, Git and a native C compiler.
"""
from __future__ import annotations
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import statistics
import sys
import tempfile
import wave

sys.dont_write_bytecode = True
import test_range_diagnostics as core

GROUPS = {
    "20261023_214834_384": dict(label="historical B-side 700 mm", clips=(24,24), expected_long=(0,0), unchanged=True),
    "20261025_154131_136": dict(label="B-side 700 mm difficult", clips=(24,24), expected_long=(2,16), unchanged=False),
    "20261025_155918_062": dict(label="A-side 700 mm", clips=(24,24), expected_long=(23,23), unchanged=True),
    "20261025_161041_655": dict(label="B-side 300 mm plus post-test extras", clips=(26,25), expected_long=(24,24), unchanged=True),
    "20261025_172902_517": dict(label="B-side 700 mm, whole boards rotated 180 degrees", clips=(24,24), expected_long=(23,23), unchanged=True),
}


def metrics(values):
    return dict(n=len(values), minimum=min(values), maximum=max(values), mean=statistics.mean(values),
                sample_sd=statistics.stdev(values) if len(values)>1 else 0) if values else dict(n=0)


def result_dict(ok, r):
    names = [name for name, _ in core.Estimate._fields_]
    return dict(ok=ok, **{name:getattr(r,name) for name in names})


def load_group(folder):
    boards = {"A":[], "B":[]}
    for path in folder.glob("*.json"):
        m=json.loads(path.read_text(encoding="utf-8-sig"))
        capture=core.load_capture(path)
        raw=path.with_suffix(".pcm").read_bytes()
        with wave.open(str(path.with_suffix(".wav")), "rb") as wav:
            assert (wav.getnchannels(),wav.getsampwidth(),wav.getnframes()) == (1,2,2048)
            assert wav.readframes(2048) == raw
        boards[m["board"]].append((path.name,m,capture))
    for vv in boards.values(): vv.sort(key=lambda v:v[2].onset_ns)
    pairs=[];used=set();extras=[]
    for a in boards["A"]:
        candidates=[b for b in boards["B"] if abs(a[2].onset_ns-b[2].onset_ns)<20_000_000]
        if not candidates:
            extras.append(dict(board="A",file=a[0],seq=a[2].sequence,onset_ns=a[2].onset_ns))
            continue
        assert len(candidates)==1, "ambiguous PTP fixture pairing"
        b=candidates[0]
        assert b[0] not in used
        used.add(b[0]);pairs.append((a,b))
    extras.extend(dict(board="B",file=b[0],seq=b[2].sequence,onset_ns=b[2].onset_ns)
                  for b in boards["B"] if b[0] not in used)
    return boards,pairs,extras


def check_snapshot_log(folder, rows):
    text=(folder/"COM3.log").read_text(encoding="utf-8-sig")
    comparisons={}
    for body in re.findall(r"RANGE CMP ([^\r\n]+)",text):
        fields=dict(re.findall(r"(\w+)=([^ ]+)",body))
        key=(int(fields["a"]),int(fields["b"]),fields["win"])
        assert key not in comparisons
        comparisons[key]=fields
    rounding=[]
    if not comparisons:
        # The earlier fixture predates CMP. All 24 LONG attempts were rejected.
        rejected=[(int(q),int(k)) for q,k in re.findall(r"quality=(\d+) lag=(-?\d+)",text)]
        assert len(rejected)==len(rows)
        assert rejected==[(r['before']['LONG']['quality_permille'],r['before']['LONG']['peak_lag_samples']) for r in rows]
        return rounding
    assert len(comparisons)==2*len(rows)
    for row in rows:
        for window in ("LONG","FRONT"):
            expected=comparisons[(row['a_seq'],row['b_seq'],window)]
            r=row['before'][window]
            integer=dict(error=r['error'],q=r['quality_permille'] if r['correlation_valid'] else None,
                         q2=r['competitor_quality_permille'] if r['competitor_valid'] else None,
                         gap=r['peak_margin_permille'] if r['competitor_valid'] else None,
                         lag=r['peak_lag_samples'] if r['correlation_valid'] else None,
                         mm=r['candidate_distance_mm'] if r['candidate_valid'] else None)
            for key,value in integer.items():
                logged=None if expected[key]=='NA' else int(expected[key])
                assert value==logged,(folder.name,row['a_seq'],window,key,value,logged)
            frac=core.ct.c_float(r['peak_fraction_samples']*65536).value
            frac=round_away(frac) if r['candidate_valid'] else None
            for key,value in [('frac',frac),('dt_ns',r['candidate_raw_delay_ns'] if r['candidate_valid'] else None)]:
                logged=None if expected[key]=='NA' else int(expected[key])
                if value!=logged:
                    assert value is not None and logged is not None and abs(value-logged)<=1
                    rounding.append(dict(seq=row['a_seq'],window=window,field=key,native=value,firmware=logged))
    return rounding


def round_away(v):
    import math
    return math.floor(v+.5) if v>=0 else math.ceil(v-.5)


def replay(folder,current,before,controlled,expect):
    boards,pairs,extras=load_group(folder)
    assert tuple(len(boards[b]) for b in ('A','B'))==expect['clips']
    assert len(pairs)==24 and len(extras)==sum(expect['clips'])-48
    rows=[]
    for a,b in pairs:
        row=dict(a_file=a[0],b_file=b[0],a_seq=a[2].sequence,b_seq=b[2].sequence,
                 a_epoch=a[2].generation,b_epoch=b[2].generation,
                 start_difference_ns=b[2].start_ns-a[2].start_ns,
                 onset_difference_ns=b[2].onset_ns-a[2].onset_ns,before={},after={})
        for window,limits in [('LONG',(128,1920)),('FRONT',(128,384))]:
            old_ok,old=core.call(before,a[2],b[2],window=limits)
            control_ok,control=core.call(controlled,a[2],b[2],window=limits)
            assert old_ok==control_ok and bytes(old)==bytes(control), (folder.name,a[2].sequence,window,'equal-width regression')
            new_ok,new=core.call(current,a[2],b[2],window=limits)
            row['before'][window]=result_dict(old_ok,old)
            row['after'][window]=result_dict(new_ok,new)
            if expect['unchanged']:
                assert old_ok==new_ok and old.error==new.error,(folder.name,a[2].sequence,window)
                if old_ok:
                    assert core.formal(old)==core.formal(new),(folder.name,a[2].sequence,window)
        rows.append(row)
    summary=dict(label=expect['label'],capture_counts={b:len(v) for b,v in boards.items()},paired_count=len(rows),extras=extras,
                 frame_integrity='all PCM CRC and WAV byte checks passed',
                 native_rounding_differences=check_snapshot_log(folder,rows),before={},after={})
    for stage in ('before','after'):
        for window in ('LONG','FRONT'):
            rr=[r[stage][window] for r in rows]
            valid=[r for r in rr if r['ok']]
            summary[stage][window]=dict(accepted=len(valid),planned=len(rows),
                errors=dict(collections.Counter(r['error'] for r in rr)),
                accepted_distance_mm=metrics([r['distance_mm'] for r in valid]),
                accepted_signed_delay_ns=metrics([r['delay_ns'] for r in valid]),
                accepted_source_counts=dict(collections.Counter('A' if r['source_a'] else 'B' for r in valid)))
    assert (summary['before']['LONG']['accepted'],summary['after']['LONG']['accepted'])==expect['expected_long']
    if folder.name=='20261025_154131_136':
        for seq in (7,22):
            row=next(r for r in rows if r['a_seq']==seq)
            assert row['before']['LONG']['ok'] and not row['after']['LONG']['ok']
            assert row['after']['LONG']['error']==4 and row['after']['LONG']['peak_margin_permille']<20
        assert all(691<=r['after']['LONG']['distance_mm']<=693 for r in rows if r['after']['LONG']['ok'])
    return summary,rows


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--baseline-ref',default='3363294')
    parser.add_argument('--captures-root',type=Path,default=core.ROOT/'build/pcm-capture')
    args=parser.parse_args()
    parent=core.ROOT/'build/algorithm-replay-validation';parent.mkdir(parents=True,exist_ok=True)
    run=Path(tempfile.mkdtemp(prefix='run-',dir=parent))
    current,before,controlled=core.build(args.cc,run,args.baseline_ref)
    results={}; missing=[]
    for name,expect in GROUPS.items():
        folder=args.captures_root/name
        if not folder.is_dir():
            missing.append(name);print('SKIP: unavailable recorded corpus',folder);continue
        summary,rows=replay(folder,current,before,controlled,expect)
        results[name]=summary
        (run/(name+'.json')).write_text(json.dumps(dict(summary=summary,rows=rows),indent=2),encoding='utf-8')
        print('PASS:',name,expect['label'],'LONG',summary['before']['LONG']['accepted'],'->',summary['after']['LONG']['accepted'],
              'FRONT',summary['before']['FRONT']['accepted'],'->',summary['after']['FRONT']['accepted'],
              'unpaired',len(summary['extras']))
    report=dict(baseline_ref=args.baseline_ref,before_search=96,after_search=127,
        capture_policy='Fixed recorded windows only. Detector changes are not simulated; missing prefixes/suffixes are never zero-filled.',
        current_source_sha256={name:hashlib.sha256((core.ROOT/'App/Ranging'/name).read_bytes()).hexdigest() for name in ('range_core.c','range_core.h')},
        groups=results,missing_groups=missing)
    (run/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print('All available fixed-capture corpus checks passed. Artifacts:',run)


if __name__=='__main__':
    main()
