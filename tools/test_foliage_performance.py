"""Render the player's distant forest view; exclude startup from throughput.

One canonical and one interpolated render per tick exercises the actual GPU
work. Uses an isolated save. --no-trees is a diagnostic control, not a fix.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--label',default='fps-after')
p.add_argument('--no-trees',action='store_true')
p.add_argument('--forest',action='store_true',help='Dense forest and grass at ground level')
p.add_argument('--display-frames',type=int,default=1)
p.add_argument('--width',type=int,default=1280)
p.add_argument('--height',type=int,default=720)
a=p.parse_args()
r=Path(__file__).resolve().parents[1]
out=r/'port/run/mod-tests/integration'
save=out/'fps-save'
shutil.copytree(r/'port/run/save',save,dirs_exist_ok=True,ignore=shutil.ignore_patterns('terrain-cache'))
if a.forest:
    path=save/'saves/1/world.json'
    data=json.loads(path.read_text())
    data['player'].update(x=12000.,y=20.25055361,z=-6000.,yaw=0.)
    path.write_text(json.dumps(data))
for frame in (60,360): (save/f'frame-{frame}.png').unlink(missing_ok=True)
env={**os.environ,'DC_TEST_LOAD_SLOT':'1','DC_TERRAIN_TEST_LOOK_AT':'0,0',
     'DC_TERRAIN_CACHE_ROOT':str(r/'port/run/save/terrain-cache'),
     'DC_PRESENT_STATS':'1','DC_TEST_CAPTURE_FRAMES':'60,360'}
if a.no_trees: env['DC_DISABLE_FOREST_MESHES']='1'
with (out/(a.label+'.log')).open('w') as log:
    result=subprocess.run([str(r/'port/build/windows-x64-release/darkcloud.exe'),
        '--data',str(r/'port/run/data'),'--save',str(save),'--offscreen',
        '--jump','edit:0','--fast-load','--frames','380','--width',str(a.width),'--height',str(a.height),
        '--display-per-tick',str(a.display_frames),'--screenshot',str(out/(a.label+'.png'))],
        cwd=r/'port/run',env=env,stdout=log,stderr=log,timeout=600)
assert result.returncode==0,result.returncode
log_text=(out/(a.label+'.log')).read_text(errors='replace')
assert 'gfx: out of' not in log_text and 'bad mesh' not in log_text and 'invalid instance range' not in log_text
seconds=(save/'frame-360.png').stat().st_mtime-(save/'frame-60.png').stat().st_mtime
print(a.label,'300 ticks seconds',seconds,'ticks/sec',300/seconds)
print('\n'.join(x for x in (out/(a.label+'.log')).read_text(errors='replace').splitlines() if x.startswith('present:')))
