"""Real rendered terrain, gate traversal, native join and cliff-top collision."""
import re, shutil
from test_town_combat import run, OUT, ROOT
cases=[
 ('gate','-700,-350,1.5707963','-600,-350,-1000,16,-350',180,150),
 ('turn','-900,-320,0','-900,-320,-900,16,0',180,170),
 ('seam','-920,-150,1.5707963','-920,-150,-1400,20,-150',240,210),
 ('exit','-1000,-150,1.5707963','-700,-150,-2000,16,-150',520,460),
 ('bend','-2550,250,0','-2550,250,-2800,16,900',210,190),
 ('plateau','1600,700,0','1600,700,1600,220,1400',210,190),
]
for name,site,camera,frames,stop in cases:
 log=run('norune-style','edit:0',frames,f'0\n40 128 0 128 128\n{stop}\n',{
  'DC_WORLD_PREVIEW_SITE':site,'DC_WORLD_PREVIEW_GROUND_CAMERA':camera,
  'DC_TOWN_WALK_TRACE':'1','DC_CANYON_DIAG':'1','DC_CANYON_COLLISION_TEST':'1',
  **({'DC_TERRAIN_MESH_OBJ':str(ROOT/'port/build/canyon-rendered.obj')} if name=='bend' else {})},timeout=600)
 points=[tuple(map(float,p)) for p in re.findall(r'town walk: ([-\d.]+) ([-\d.]+) ([-\d.]+)',log)]
 assert points, name
 assert '0 mismatches' in log, name
 assert all(y>=-1 for x,y,z in points),(name,points)
 x,z,_=map(float,site.split(','))
 assert (points[-1][0]-x)**2+(points[-1][2]-z)**2>100**2,(name,points)
 if name=='gate':
  assert points[-1][0]<-850,points
  assert re.findall(r'canyon program (\d+)',log)==['128'],log[-3000:]
 if name=='turn':
  assert points[-1][2]>-160,points
  assert re.findall(r'canyon program (\d+)',log)==['128'],log[-3000:]
 if name=='seam':
  assert points[-1][0]<-1150 and all(abs(y)<1 for x,y,z in points),points
 if name=='exit':assert points[-1][0]<-1600,points
 shutil.copyfile(OUT/'norune-style.png',OUT/f'norune-collision-{name}.png')
 shutil.copyfile(OUT/'norune-style.log',OUT/f'norune-collision-{name}.log')
 print('PASS',name,points[-1],flush=True)
