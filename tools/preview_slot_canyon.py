"""Fast layout preview using the retail mesh dump and production terrain kernel.
Run after exporting DC_TOWN_BOUNDARY_OBJ once; never loads the game itself.
"""
from pathlib import Path
import subprocess
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
from matplotlib.colors import LightSource, Normalize
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'port/build'
verts=[];faces=[];name=''
for line in (OUT/'norune-boundary.obj').read_text().splitlines():
 a=line.split()
 if not a: continue
 if a[0]=='o':name=a[1]
 if a[0]=='v':verts.append(list(map(float,a[1:4])))
 if a[0]=='f' and name.startswith('waku'):faces.append((name,[int(i)-1 for i in a[1:]]))
v=np.array(verts)
raster=np.zeros((641,641))
for name,ids in faces:
 t=v[ids]
 a,b,c=t;den=(b[2]-c[2])*(a[0]-c[0])+(c[0]-b[0])*(a[2]-c[2])
 if abs(den)<1e-6:continue
 lo=np.maximum(0,np.floor((t[:,[0,2]].min(axis=0)+1280)/4).astype(int))
 hi=np.minimum(640,np.ceil((t[:,[0,2]].max(axis=0)+1280)/4).astype(int))
 xx,zz=np.meshgrid(np.arange(lo[0],hi[0]+1),np.arange(lo[1],hi[1]+1))
 x=xx*4-1280;z=zz*4-1280
 u=((b[2]-c[2])*(x-c[0])+(c[0]-b[0])*(z-c[2]))/den
 w=((c[2]-a[2])*(x-c[0])+(a[0]-c[0])*(z-c[2]))/den
 h=np.where((u>=0)&(w>=0)&(u+w<=1),u*a[1]+w*b[1]+(1-u-w)*c[1],0)
 raster[zz,xx]=np.maximum(raster[zz,xx],h)
a=np.arange(512)[:,None]*2*np.pi/512;r=np.arange(257)[None,:]*8
x=(np.cos(a)*r+1280)/4;z=(np.sin(a)*r+1280)/4
ix=np.floor(x).astype(int);iz=np.floor(z).astype(int);u=x-ix;w=z-iz
valid=(ix>=0)&(iz>=0)&(ix<640)&(iz<640);ix=ix.clip(0,639);iz=iz.clip(0,639)
h=(raster[iz,ix]*(1-u)+raster[iz,ix+1]*u)*(1-w)+(raster[iz+1,ix]*(1-u)+raster[iz+1,ix+1]*u)*w
h=np.maximum.accumulate(np.where(valid,h,0),axis=1)
h.astype('<f4').tofile(OUT/'norune-shell.bin')
compiler=ROOT/'.kazarov/windows/deps/llvm/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe'
subprocess.run([str(compiler),'-std=c++20','-O2','-static','-Iport/include','tools/preview_slot_canyon.cpp','-o',str(OUT/'preview_slot_canyon.exe')],cwd=ROOT,check=True)
subprocess.run([str(OUT/'preview_slot_canyon.exe')],cwd=ROOT,check=True)
d=np.genfromtxt(OUT/'slot-canyon-map.csv',delimiter=',',names=True)
xs=np.unique(d['x']);zs=np.unique(d['z']);height=d['height'].reshape(len(zs),len(xs))
cmap=plt.colormaps['terrain'];norm=Normalize(-30,270)
shade=LightSource(315,50).shade(height,cmap=cmap,norm=norm,vert_exag=1,dx=48,dy=48)
fig,axs=plt.subplots(1,2,figsize=(16,10),gridspec_kw={'width_ratios':[1.1,1]})
for ax in axs:
 ax.imshow(shade,origin='lower',extent=[xs.min(),xs.max(),zs.min(),zs.max()])
 # Exact native faces laid over the height field, including the original platform.
 poly=PolyCollection([v[f][:,[0,2]] for _,f in faces],array=np.array([v[f,1].mean() for _,f in faces]),cmap=cmap,norm=norm,edgecolors=(0,0,0,.15),linewidths=.2)
 ax.add_collection(poly);ax.set_aspect('equal');ax.set_xlabel('World X (10 units = 1 metre)');ax.set_ylabel('World Z')
axs[0].set(title='Slot-canyon district: flat floors and ~20 m walls',xlim=(-24000,26000),ylim=(-21000,21000))
axs[0].annotate('Original Norune',xy=(0,0),xytext=(1000,2400),arrowprops={'arrowstyle':'->'},ha='center',fontsize=12)
axs[1].set(title='Original town + western exit connection',xlim=(-5200,1700),ylim=(-1900,4700))
axs[1].add_patch(plt.Rectangle((-700,-700),1400,1200,fill=False,color='cyan',lw=1.5))
axs[1].annotate('Existing exit\ncontinues into canyon',xy=(-1200,-150),xytext=(-2700,1050),arrowprops={'arrowstyle':'->'},fontsize=11)
fig.suptitle('Layout preview • blue/green = low ground, tan = cliff tops • colors are elevation, not water',fontsize=14)
fig.colorbar(plt.cm.ScalarMappable(norm=norm,cmap=cmap),ax=axs,shrink=.45,label='Height (game units)')
fig.savefig(OUT/'slot-canyon-preview.png',dpi=140,bbox_inches='tight')
print(OUT/'slot-canyon-preview.png')

