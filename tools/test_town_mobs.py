"""Real retail mob models, spawning and animation in a copied player save."""
from pathlib import Path
import os,re,shutil,subprocess
r=Path(__file__).resolve().parents[1]
out=r/'port/run/mod-tests/integration'
save=out/'night-mobs-save'
shutil.copytree(r/'port/run/save',save,dirs_exist_ok=True,ignore=shutil.ignore_patterns('terrain-cache'))
env={**os.environ,'DC_TEST_LOAD_SLOT':'1','DC_TEST_REALTIME':'1','DC_TEST_MOBS_NIGHT':'1',
     'DC_MOBS_TRACE':'1','DC_TERRAIN_PERF':'1','DC_TEST_CAPTURE_FRAMES':'300,800,1100',
     'DC_TEST_MOBS_VIEW':'1','DC_TEST_MOBS_CYCLE':'1',
     'DC_TERRAIN_CACHE_ROOT':str(r/'port/run/save/terrain-cache')}
with (out/'night-mobs.log').open('w') as log:
    p=subprocess.run([str(r/'port/build/windows-x64-release/darkcloud.exe'),'--data',str(r/'port/run/data'),
        '--save',str(save),'--offscreen','--jump','edit:0','--fast-load','--frames','1500'],
        cwd=r/'port/run',env=env,stdout=log,stderr=log,timeout=600)
assert p.returncode==0,p.returncode
s=(out/'night-mobs.log').read_text(errors='replace')
assert 'loaded Skeleton Soldier, Dasher, Cave Bat' in s
spawns=re.findall(r'spawn kind (\d).*distance ([\d.]+) count (\d+)',s)
assert len(spawns)>=8,len(spawns)
assert {int(k) for k,_,_ in spawns}=={0,1,2}
assert all(480<float(d)<=1280 and int(n)<=14 for _,d,n in spawns)
assert max(int(n) for _,_,n in spawns)==14,'Population never exercised the cap'
assert 'night mobs: spawn' not in s.split('night mobs: test dawn',1)[1]
assert 'despawn distance' in s.split('night mobs: test travel',1)[1]
assert re.search(r'despawn distance [\d.]+ count 0',s.split('night mobs: test travel',1)[1])
print('PASS native models and night packs; spawns',len(spawns),'peak',max(int(n) for _,_,n in spawns))
print('PASS dawn stops spawning, travel despawns population')

# Stay at the same location through eight seconds of daylight. This exercises
# the real 20 Hz timer and all three HP values, not only the arithmetic helper.
env['DC_TEST_MOBS_SUN']='1'
env['DC_TEST_CAPTURE_FRAMES']='1300,1700'
with (out/'night-mobs-sun.log').open('w') as log:
    p=subprocess.run([str(r/'port/build/windows-x64-release/darkcloud.exe'),'--data',str(r/'port/run/data'),
        '--save',str(save),'--offscreen','--jump','edit:0','--fast-load','--frames','1850'],
        cwd=r/'port/run',env=env,stdout=log,stderr=log,timeout=600)
assert p.returncode==0,p.returncode
s=(out/'night-mobs-sun.log').read_text(errors='replace')
histories={}
for kind,hp,slot in re.findall(r'daylight kind (\d) hp (-?\d+) slot (\d+)',s):
    histories.setdefault((int(kind),int(slot)),[]).append(int(hp))
assert {k for k,_ in histories}=={0,1,2}
for (kind,slot),health in histories.items():
    maximum=12 if kind==2 else 23
    assert health==[maximum-maximum*second//8 for second in range(1,len(health)+1)],(kind,slot,health)
    assert len(health)<=8
completed=[key for key,health in histories.items() if len(health)==8]
assert {kind for kind,slot in completed}=={0,1,2}
dawn=s.split("night mobs: test dawn",1)[1]
despawns=len(re.findall(r"night mobs: despawn distance",dawn))
assert len(completed)+despawns==int(re.search(r"tick 400 night 0 count (\d+)",s)[1]), (len(completed),despawns)
assert 'night mobs: spawn' not in s.split('night mobs: test dawn',1)[1]
assert re.search(r'tick 600 night 0 count 0',s)
print('PASS daylight:',len(completed),'enemies die after exactly eight one-second damage ticks;',despawns,'walk beyond despawn radius')
