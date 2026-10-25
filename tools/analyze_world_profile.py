"""Summarize DC_WORLD_PROFILE JSONL; export a Perfetto-compatible timeline.
Durations are elapsed scope times (not OS CPU cycles). Exclusive times remove
nested instrumented spans on each thread; wait spans are reported separately.
"""
import argparse
import collections
import json
from pathlib import Path


def stats(values):
    v=sorted(values)
    if not v:return {}
    return dict(count=len(v),total_ms=sum(v)/1000,mean_ms=sum(v)/len(v)/1000,
                p50_ms=v[int((len(v)-1)*.5)]/1000,p95_ms=v[int((len(v)-1)*.95)]/1000,
                p99_ms=v[int((len(v)-1)*.99)]/1000,max_ms=v[-1]/1000)


def summarize(events):
    groups=collections.defaultdict(list)
    threads=collections.defaultdict(list)
    counters=collections.defaultdict(list)
    for e in events:
        if e.get('ph')=='X':threads[e['tid']].append(e)
        elif e.get('ph')=='C':counters[e['name']].append(e['args']['value'])
    for spans in threads.values():
        spans.sort(key=lambda e:(e['ts'],-e['dur']))
        stack=[]
        for e in spans:
            start,end=e['ts'],e['ts']+e['dur']
            while stack and start>=stack[-1][0]:stack.pop()
            # Wait intervals may overlap running scopes and are not CPU children.
            if e.get('cat')=='wait':
                groups[(e['name'],'wait',e.get('args',{}).get('lod',-1))].append([e['dur'],e['dur']]);continue
            while stack and end>stack[-1][0]+.01:stack.pop()
            row=[e['dur'],e['dur']]
            if stack:stack[-1][1][1]-=e['dur']
            groups[(e['name'],'cpu',e.get('args',{}).get('lod',-1))].append(row)
            stack.append((end,row))
    builds=collections.defaultdict(list)
    for e in events:
        if e.get('ph')=='X' and e['name']=='world.build.total':
            a=e.get('args',{});builds[a.get('lod',-1)].append((a.get('x'),a.get('z')))
    churn={str(lod):dict(builds=len(v),unique_tiles=len(set(v)),builds_per_tile=len(v)/len(set(v))) for lod,v in builds.items()}
    rows=[]
    for (name,category,lod),values in groups.items():
        rows.append(dict(name=name,category=category,lod=lod,inclusive=stats([v[0] for v in values]),
                         exclusive=stats([max(0,v[1]) for v in values])))
    rows.sort(key=lambda r:r['exclusive']['total_ms'],reverse=True)
    return dict(build_churn=churn,scopes=rows,counters={k:dict(count=len(v),minimum=min(v),maximum=max(v),mean=sum(v)/len(v),last=v[-1]) for k,v in counters.items()},
                gpu_draw=stats(counters.get('gpu.draw_frame_us',[])))


def analyze(path):
    events=[];bad=0
    with path.open(encoding='utf-8') as f:
        for line in f:
            try:events.append(json.loads(line))
            except json.JSONDecodeError:bad+=1
    result=summarize(events);result['invalid_lines']=bad
    # Flight-only summary excludes startup, teleport loading and preflight views.
    markers=[e['ts'] for e in events if e.get('ph')=='C' and e['name']=='world.flight' and e['args']['value']]
    if markers:result['flight']=summarize([e for e in events if e['ts']>=min(markers)])
    path.with_suffix('.summary.json').write_text(json.dumps(result,indent=2))
    path.with_suffix('.trace.json').write_text(json.dumps({'traceEvents':events},separators=(',',':')))
    lines=['Elapsed scope time; worker totals overlap across threads. GPU draw excludes upload command buffers.',
           'Exclusive scope milliseconds: name | LOD | count | total | p95 | max']
    for label,data in [('All',result),('Flight',result.get('flight',{}))]:
        if not data:continue
        lines.append('\n'+label)
        for category in ('cpu','wait'):
            lines.append(category.upper())
            for r in [r for r in data['scopes'] if r['category']==category][:25]:
                s=r['exclusive'];lines.append(f"{r['name']} | {r['lod']} | {s['count']} | {s['total_ms']:.1f} | {s['p95_ms']:.3f} | {s['max_ms']:.3f}")
        lines.append('GPU draw: '+json.dumps(data['gpu_draw']))
        lines.append('Build churn by LOD: '+json.dumps(data['build_churn']))
    text='\n'.join(lines);path.with_suffix('.summary.txt').write_text(text);print(text)
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('capture',type=Path);analyze(p.parse_args().capture)
