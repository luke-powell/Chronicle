"""Validate captured Norune surfaces against the native-only render and audit UV scale."""
from pathlib import Path
from collections import defaultdict
import json
import re
import numpy as np
from PIL import Image
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'port/run/mod-tests/integration'
a=np.asarray(Image.open(OUT/'norune-style-native.png').convert('RGB')).astype(int)
b=np.asarray(Image.open(OUT/'norune-style-town.png').convert('RGB')).astype(int)
report={'capture_comparison':{},'native_materials':{}}
for name,box in {'cliffs_house_platform':(0,320,1280,460),'floor':(0,480,1280,530)}.items():
 x0,y0,x1,y1=box;error=np.abs(a[y0:y1,x0:x1]-b[y0:y1,x0:x1])
 report['capture_comparison'][name]={'mean_rgb_error':float(error.mean()),'changed_pixels':int(np.count_nonzero(error.max(axis=2)))}
 assert error.max()==0,(name,error.max())
v=[];uv=[];obj='';mat='';stats=defaultdict(list);floor_ids=set()
for line in (ROOT/'port/build/norune-boundary.obj').read_text().splitlines():
 t=line.split()
 if not t:continue
 if t[0]=='o':obj=t[1]
 if t[0]=='v':v.append(list(map(float,t[1:4])))
 if t[0]=='vt':uv.append(list(map(float,t[1:3])))
 if t[0]=='usemtl':mat=t[1]
 if t[0]=='f' and obj.startswith('waku'):
  ids=[int(i)-1 for i in t[1:]];p=np.array([v[i] for i in ids]);q=np.array([uv[i] for i in ids])
  if mat=='e01b02':floor_ids.update(ids)
  area=np.linalg.norm(np.cross(p[1]-p[0],p[2]-p[0]))/2;u=q[1]-q[0];w=q[2]-q[0];tex=abs(u[0]*w[1]-u[1]*w[0])/2
  if area>.01 and tex>.0001:stats[mat].append((area/tex)**.5)
for material in ['e01b01','e01b02','e01b06','e01b05']:
 report['native_materials'][material]={'median_world_units_per_texture_repeat':float(np.median(stats[material])),'measured_triangles':len(stats[material])}
# The outlet uses e01b02, not the centre-of-town e01b01. Compare the actual
# renderer mapping to both ends of the authored open floor edge.
source=(ROOT/'port/src/town_terrain.cpp').read_text()
u=list(map(float,re.search(r'constexpr double ux=([^,;]+),uz=([^,;]+),uo=([^;]+)',source).groups()))
w=list(map(float,re.search(r'constexpr double vx=([^,;]+),vz=([^,;]+),vo=([^;]+)',source).groups()))
errors=[]
for i in floor_ids:
 p,q=v[i],uv[i]
 if abs(p[1])<.001 and (np.linalg.norm(np.array(p)-[-1140.72,0,-186.24])<.01 or np.linalg.norm(np.array(p)-[-1142.36,0,-93.1199])<.01):
  predicted=np.array([[p[0],p[2],1]])@np.array([u,w]).T
  errors.append(float(np.max(np.abs(predicted-q))))
assert errors and max(errors)<.0001,errors
report['exit_uv_max_error']=max(errors)
(OUT/'norune-style-evidence.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
