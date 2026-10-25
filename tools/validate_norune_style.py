"""Validate captured Norune surfaces against the native-only render and audit UV scale."""
from pathlib import Path
from collections import defaultdict
import json
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
v=[];uv=[];obj='';mat='';stats=defaultdict(list)
for line in (ROOT/'port/build/norune-boundary.obj').read_text().splitlines():
 t=line.split()
 if not t:continue
 if t[0]=='o':obj=t[1]
 if t[0]=='v':v.append(list(map(float,t[1:4])))
 if t[0]=='vt':uv.append(list(map(float,t[1:3])))
 if t[0]=='usemtl':mat=t[1]
 if t[0]=='f' and obj.startswith('waku'):
  ids=[int(i)-1 for i in t[1:]];p=np.array([v[i] for i in ids]);q=np.array([uv[i] for i in ids])
  area=np.linalg.norm(np.cross(p[1]-p[0],p[2]-p[0]))/2;u=q[1]-q[0];w=q[2]-q[0];tex=abs(u[0]*w[1]-u[1]*w[0])/2
  if area>.01 and tex>.0001:stats[mat].append((area/tex)**.5)
for material in ['e01b01','e01b06','e01b05']:
 report['native_materials'][material]={'median_world_units_per_texture_repeat':float(np.median(stats[material])),'measured_triangles':len(stats[material])}
(OUT/'norune-style-evidence.json').write_text(json.dumps(report,indent=2))
print(json.dumps(report,indent=2))
