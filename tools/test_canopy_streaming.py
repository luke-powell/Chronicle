"""Forests must be ready with terrain, including after turning and far travel."""
import re
from test_town_combat import run

log=run('forest-owned-turn','edit:0',720,
        '0\n300 128 128 255 128\n420\n500 128 128 0 128\n620\n',
        {'DC_FOREST_TRACE':'1','DC_TERRAIN_PERF':'1'})
ready=re.findall(r'terrain vegetation ready: level (\d+) canopy (\d+) forestMask (\d+)',log)
assert ready and all(int(c)==1 and (int(l)>3 or int(m)&4) for l,c,m in ready)
assert max(int(l) for l,_,_ in ready)>10
first=log.index('terrain draw:')
assert 'terrain forest ready:' in log[:first]
assert 'forest page:' not in log and 'forest draw:' not in log
updates=re.findall(r'terrain perf: update [\d.]+ ms built (\d+).*pending (\d+)',log)
assert updates and all(int(n)==0 and int(p)==0 for n,p in updates[-100:])
print('PASS first visible terrain has tree geometry; canopy exists at every LOD; no rotation rebuild queue',flush=True)

log=run('original-orbit-up','edit:0',600,
        '0\n220 128 128 128 0\n500\n',{'DC_CAMERA_PITCH_TRACE':'1'})
rows=[tuple(map(float,r)) for r in re.findall(r'walking orbit: height ([-\d.]+) distance ([-\d.]+)',log)]
assert rows and min(r[0] for r in rows)<0 and max(r[1] for r in rows)-min(r[1] for r in rows)<1,rows[-10:]
assert 'walking look:' not in log
print('PASS original third-person orbit can look above horizon without changing follow distance',flush=True)

log=run('forest-owned-trillion','edit:0',240,'0\n',
        {'DC_TERRAIN_TEST_SPAWN':'999999997952,-999999997952,0','DC_FOREST_TRACE':'1','DC_TERRAIN_TRACE':'1'})
ready=re.findall(r'terrain vegetation ready: level (\d+) canopy (\d+) forestMask (\d+)',log)
assert ready and all(int(c)==1 and (int(l)>3 or int(m)&4) for l,c,m in ready)
assert '999999997952' in log
print('PASS terrain-owned forest and all canopy LODs at trillion-unit coordinates',flush=True)
