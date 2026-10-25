"""Fresh save view, look-away/return, grass close-up and sustained Dran travel.

Uses a copy of the player's save. DC_TEST_EXE can select an isolated candidate
executable. Screenshots/logs remain in port/run/mod-tests/integration.
"""
import os
import re
import shutil
import subprocess
from pathlib import Path

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'port/run/mod-tests/integration'
SAVE=OUT/'world-acceptance-save'
OUT.mkdir(parents=True,exist_ok=True)
shutil.copytree(ROOT/'port/run/save',SAVE,dirs_exist_ok=True,ignore=shutil.ignore_patterns('terrain-cache'))
for frame in (60,150,290,450,900,1600,1850):
    (SAVE/f'frame-{frame}.png').unlink(missing_ok=True)
inputs=OUT/'world-acceptance.input'
inputs.write_text('0\n1000 r3\n1001 cross\n1050 r1 128 0 128 128\n1700\n',encoding='ascii')
env={**os.environ,'DC_TEST_LOAD_SLOT':'1','DC_TERRAIN_TEST_LOOK_AT':'0,0',
     'DC_TERRAIN_TEST_LOOK_SWEEP':'1','DC_TERRAIN_TEST_SPAWN':'12000,-6000,0',
     'DC_TERRAIN_TEST_SPAWN_FRAME':'620','DC_TERRAIN_PERF':'1','DC_FOREST_TRACE':'1',
     'DC_DRAN_TRACE':'1','DC_TEST_REALTIME':'1','DC_PRESENT_STATS':'1','DC_TEST_CAPTURE_FRAMES':'60,150,290,450,900,1600,1850'}
env['DC_TERRAIN_CACHE_ROOT']=str(ROOT/'port/run/save/terrain-cache')
exe=os.environ.get('DC_TEST_EXE',str(ROOT/'port/build/windows-x64-release/darkcloud.exe'))
with (OUT/'world-acceptance.log').open('w') as log:
    result=subprocess.run([exe,'--data',str(ROOT/'port/run/data'),'--save',str(SAVE),
        '--offscreen','--jump','edit:0','--fast-load','--frames','1900','--input',str(inputs),
        '--width','1280','--height','720','--screenshot',str(OUT/'world-acceptance.png')],
        cwd=ROOT/'port/run',env=env,stdout=log,stderr=log,timeout=900)
assert result.returncode==0,result.returncode
text=(OUT/'world-acceptance.log').read_text(errors='replace')
first_draw=text.index('terrain draw:')
assert 'level 6 canopy 1 forestMask 4' in text[:first_draw]
assert 'level 7 canopy 1 forestMask 4' in text[:first_draw]
flight=text.split('dran: takeoff',1)[1]
assert 'initial 1' not in flight,'Flight blocked for a full world rebuild'
assert 'terrain publish local:' in flight or 'terrain publish: focus' in flight,'No terrain published during flight'
centres=[int(level) for level in re.findall(r'terrain centre: level (-?\d+)',flight)]
assert centres and max(centres)==0 and min(centres)==0,sorted(set(centres))
updates=sorted(float(t) for t in re.findall(r'update ([\d.]+) ms.*initial 0',flight))
assert updates and updates[int(len(updates)*.99)]<20,updates[-20:]
for frame in (60,150,290,450,900,1600,1850):assert (SAVE/f'frame-{frame}.png').read_bytes().startswith(b'\x89PNG')
print('PASS first-view distant tree geometry, full-resolution close-up, flight publication, no synchronous flight rebuild')
print('flight terrain update median / p99 / max ms:',updates[len(updates)//2],updates[int(len(updates)*.99)],updates[-1])
print('Visual evidence:',SAVE)

