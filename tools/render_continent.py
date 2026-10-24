import sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource
p=Path(sys.argv[1]);d=np.genfromtxt(p,delimiter=',',names=True);n=int(np.sqrt(len(d)));h=d['height'].reshape(n,n)
colors=np.zeros((n,n,3))+[.56,.62,.33]
sand=d['sand'].reshape(n,n,1);snow=d['snow'].reshape(n,n,1);density=d['density'].reshape(n,n,1)
colors=colors*(1-sand)+np.array([.79,.66,.39])*sand
forest=np.minimum(density*1.8,1)
colors=colors*(1-forest)+np.array([.12,.29,.15])*forest
rock=np.clip((d['slope'].reshape(n,n,1)-22)/25,0,1)
colors=colors*(1-rock)+np.array([.40,.38,.32])*rock
colors=colors*(1-snow)+np.array([.91,.93,.92])*snow
sea=h < -300;colors[sea]=np.array([.12,.30,.43])
spacing=abs(d['x'][1]-d['x'][0]);light=LightSource(315,40);shade=light.hillshade(h,vert_exag=3,dx=spacing,dy=spacing)
colors*=.55+.6*shade[:,:,None];colors[sea]=[.10,.26,.37]
extent=(d['x'].min()/10000,d['x'].max()/10000,d['z'].max()/10000,d['z'].min()/10000)
fig,axes=plt.subplots(1,3,figsize=(21,8),constrained_layout=True)
axes[0].imshow(np.clip(colors,0,1),extent=extent);axes[0].set_title('Continents, ranges & biomes')
axes[1].imshow(h,extent=extent,cmap='terrain',vmin=-4000,vmax=24000);axes[1].set_title('Elevation and coastlines')
axes[2].imshow(density[:,:,0],extent=extent,cmap='YlGn',vmin=0,vmax=.8);axes[2].set_title('Vegetation: stands, gaps & open land')
for ax in axes:ax.set_xlabel('World X (km)');ax.set_ylabel('World Z (km)')
fig.suptitle('Actual world generator - 1 game unit = 0.1 metre',fontsize=18)
fig.savefig(p.with_suffix('.png'),dpi=115);print(p.with_suffix('.png'))
