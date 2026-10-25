"""Exercise overhead LOD without touching the player's save."""
import os, re, shutil, subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[1]
out=root/'port/run/mod-tests/integration'
save=out/'rings-overhead-save'
shutil.copytree(root/'port/run/save',save,dirs_exist_ok=True,ignore=shutil.ignore_patterns('terrain-cache'))
inputs=out/'rings-overhead.input'
inputs.write_text('0\n100 select\n101\n140 l2\n650\n700 r2\n1000\n')
env={**os.environ,'DC_TEST_LOAD_SLOT':'1','DC_TERRAIN_PERF':'1','DC_FOREST_TRACE':'1',
 'DC_TERRAIN_CACHE_ROOT':str(root/'port/run/save/terrain-cache'),
 'DC_TEST_CAPTURE_FRAMES':'60,350,680,1050'}
with (out/'rings-overhead.log').open('w') as log:
 result=subprocess.run([str(root/'port/build/windows-x64-release/darkcloud.exe'),
 '--data',str(root/'port/run/data'),'--save',str(save),'--offscreen','--jump','edit:0',
 '--fast-load','--frames','1080','--width','1280','--height','720','--input',str(inputs)],
 cwd=root/'port/run',env=env,stdout=log,stderr=log,timeout=600)
assert result.returncode==0
text=(out/'rings-overhead.log').read_text(errors='replace')
assert 'out of mesh' not in text
rows=re.findall(r'terrain perf:.*chunks (\d+) cached.*lod (\d+) initial (\d+)',text)
assert rows and all(int(count)==928 and int(lod)==0 for count,lod,initial in rows),set(rows)
assert sum(int(initial) for count,lod,initial in rows)==1
for frame in (60,350,680,1050):assert (save/f'frame-{frame}.png').exists()
print('PASS zoom out/in: 928 resident tiles on every frame; full-resolution centre; no world rebuild')
