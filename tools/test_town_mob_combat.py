"""Native combat state, input lock-on, hurt and death regression in a copied save."""
from pathlib import Path
import os,subprocess,shutil,re
r=Path(__file__).resolve().parents[1];out=r/'port/run/mod-tests/integration';save=out/'mob-combat-save'
shutil.copytree(r/'port/run/save',save,dirs_exist_ok=True,ignore=shutil.ignore_patterns('terrain-cache'))
script=out/'mob-combat.input';script.write_text('0\n210 r3\n211\n660 r3\n661\n1110 r3\n1111\n')
env={**os.environ,'DC_TEST_LOAD_SLOT':'1','DC_TEST_REALTIME':'1','DC_TEST_MOBS_NIGHT':'1','DC_MOBS_TRACE':'1','DC_TEST_MOBS_COMBAT':'1','DC_TOWN_COMBAT_TRACE':'1','DC_TEST_CAPTURE_FRAMES':'240,430,510,690,880,960,1140,1330,1410,1740','DC_TERRAIN_CACHE_ROOT':str(r/'port/run/save/terrain-cache'),'DC_AUDIO_WAV':str(out/'mob-combat.wav')}
with (out/'mob-combat.log').open('w') as log:
 p=subprocess.run([str(r/'port/build/windows-x64-release/darkcloud.exe'),'--data',str(r/'port/run/data'),'--save',str(save),'--offscreen','--jump','edit:0','--fast-load','--frames','1800','--input',str(script)],cwd=r/'port/run',env=env,stdout=log,stderr=log,timeout=600)
assert p.returncode==0,p.returncode
s=(out/'mob-combat.log').read_text(errors='replace')
for kind in range(3):
 assert re.search(r'attack impact kind '+str(kind)+r' damage [1-9]',s),kind
 assert 'attack missed kind '+str(kind) in s,kind
 assert 'hurt kind '+str(kind) in s,kind
 assert 'death kind '+str(kind) in s,kind
 death=[float(t) for t in re.findall(r'test death kind '+str(kind)+r' motion \d+ time ([\d.]+)',s)]
 assert len(death)>=2 and max(death)>min(death),(kind,death)
 times=[float(t) for t in re.findall(r'test reaction kind '+str(kind)+r' motion 8 time ([\d.]+)',s)]
 assert len(times)>=2 and times[-1]>times[0],(kind,times)
assert s.count('test interrupted action 2')==3
assert 'night mobs: lock 0' in s
assert 'impact sound 20' in s and 'impact sound 21' in s
print('PASS all three species: real player damage, dodge miss, sword interrupt, animated hit/death, R3 lock-on, impact audio')


peaks=re.findall(r'audio verify note (\d+) peak ([\d.]+)',s)
assert {int(note) for note,peak in peaks if float(peak)>.0001}=={20,21,23},peaks
assert 'test knockout hp 0' in s and 'knockout recovery at arrival position' in s
print('PASS isolated PCM render of hit/player/block sounds and knockout recovery')
