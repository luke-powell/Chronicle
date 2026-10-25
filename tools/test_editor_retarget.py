"""Real editor input: slope exit, then rapid distant pan/reversal/zoom/exit.
Uses copied saves. The slope failed the fourth collision probe before the fix.
"""
from pathlib import Path
import os,re,shutil,subprocess
root=Path(__file__).resolve().parents[1]
out=root/'port/run/mod-tests/integration'

def run(label,inputs,frames,extra):
    save=out/(label+'-save');shutil.copytree(root/'port/run/save',save,dirs_exist_ok=True,ignore=shutil.ignore_patterns('terrain-cache'))
    script=out/(label+'.input');script.write_text(inputs)
    env={**os.environ,'DC_TEST_LOAD_SLOT':'1','DC_TEST_REALTIME':'1','DC_TERRAIN_TRACE':'1','DC_TERRAIN_PERF':'1',
         'DC_TERRAIN_CACHE_ROOT':str(root/'port/run/save/terrain-cache'),**extra}
    path=out/(label+'.log')
    with path.open('w') as log:
        result=subprocess.run([str(root/'port/build/windows-x64-release/darkcloud.exe'),'--data',str(root/'port/run/data'),
          '--save',str(save),'--offscreen','--jump','edit:0','--fast-load','--frames',str(frames),'--input',str(script)],
          cwd=root/'port/run',env=env,stdout=log,stderr=log,timeout=600)
    assert result.returncode==0,result.returncode
    return path.read_text(errors='replace')

slope=run('editor-slope','0\n100 select\n101\n180 select\n181\n',260,
          {'DC_WORLD_PREVIEW_SITE':'10000,289818,0','DC_TERRAIN_DIAGNOSTIC':'geometry'})
assert 'landing check: sample 3' in slope and 'editor: walk resumed' in slope
print('PASS uphill landing: all four probes hit; editor returns to walking',flush=True)

pan=run('editor-retarget','0\n100 select\n101\n140 l2\n460\n480 255 128 128 128\n500 0 128 128 128\n510\n550 r2\n890\n950 select\n951\n',1800,{'DC_TEST_CAPTURE_FRAMES':'940,1100'})
assert 'obsolete local plan discarded' in pan
assert 'editor: walk resumed' in pan,'Landing did not complete at the new destination'
publications=re.findall(r'publish local: focus ([-\d]+) ([-\d]+) chunks (\d+) plan ([-\d]+) ([-\d]+)',pan)
assert publications
for x,z,n,px,pz in publications:
    assert int(n)==928
    assert max(abs(int(x)-int(px)),abs(int(z)-int(pz)))<=2048,(x,z,px,pz)
assert sum(int(v) for v in re.findall(r'terrain perf:.*initial (\d+)',pan))==1,'Editor pan blocked on a world rebuild'
assert 'road cache: build' in pan,'Saved roads did not exercise the drape cache'
travel=pan[pan.index('terrain: retarget'):]
assert 'road cache: build' not in travel,'Origin shifts rebuilt unchanged road meshes'
assert not re.search(r'invalidated \d+ of',travel),'Camera travel invalidated unchanged terrain'
print('PASS rapid pan/reversal/zoom: no obsolete local publication, fixed budget, landing completes')
ticks=sorted(map(float,re.findall(r'town tick: ([\d.]+) ms mode 4',pan)))
print('Editor CPU tick median / p99 / max ms:',ticks[len(ticks)//2],ticks[int(.99*len(ticks))],max(ticks))
print('PASS stable road cache across origin shifts')
