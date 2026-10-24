"""Real-game forest population/LOD and distant landing regression checks."""
import re
from test_town_combat import run, OUT

log=run('forest-lod-zoom','edit:0',1000,
        '0\n220 select\n221\n250 l2\n400\n',
        {'DC_TERRAIN_TEST_SPAWN':'16050,0,0','DC_FOREST_TRACE':'1','DC_TERRAIN_PERF':'1'})
models=re.findall(r'forest model: species (\d+) lod (\d+) triangles (\d+)',log)
assert len(models)==15 and all(int(n)>0 for _,_,n in models),models
pages={}
for x,z,level,count,ids,mask in re.findall(r'terrain forest ready: (-?\d+) (-?\d+) level (\d+) trees (\d+) ids (\d+) mask (\d+)',log):
    key=(x,z,level)
    value=(int(count),int(ids))
    assert key not in pages or pages[key]==value,(key,pages.get(key),value)
    pages[key]=value
    assert int(mask)==(7 if int(level)<=2 else 4)
assert len(pages)>10 and sum(c for c,_ in pages.values())>1000
ready=re.findall(r'terrain vegetation ready: level (\d+) canopy (\d+) forestMask (\d+)',log)
assert ready and all(int(c)==1 and (int(l)>3 or int(m)&4) for l,c,m in ready)
print(f'PASS {len(pages)} terrain chunks have deterministic tree populations and complete LODs before display')
log=run('forest-fast-land','edit:0',1200,
        '0\n220 select\n221\n250 l2\n580\n600 255 128 128 128\n780\n800 select\n801\n',
        {'DC_FOREST_TRACE':'1','DC_TERRAIN_TRACE':'1'})
ms=float(re.search(r'instant landing ([\d.]+) ms',log)[1])
assert ms<2000,ms
assert 'editor: walk resumed' in log
positions=[tuple(map(float,p)) for p in re.findall(r'terrain: player ([-\d.]+) ([-\d.]+) ([-\d.]+) ground ([-\d.]+)',log)]
x,y,z,h=positions[-1]
assert abs(z)>1_000_000 and abs(y-h)<1,(x,y,z,h)
print(f'PASS distant landing: {ms:.1f} ms; grounded at ({x:.1f}, {z:.1f})')
