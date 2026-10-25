"""Compare actual kernel geometry, not textures, at hill and mountain scales."""
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource
p=Path('port/run/mod-tests/integration')
fig,axs=plt.subplots(2,3,figsize=(15,9))
for row,kind in enumerate(['hill','mountain']):
 d=np.genfromtxt(p/f'weathering-{kind}-map.csv',delimiter=',',names=True);n=128
 spacing=(d['x'][1]-d['x'][0])*.1;ext=[0,n*spacing,0,n*spacing]
 for col,key in enumerate(['before','after']):
  h=d[key].reshape(n,n)*.1
  dz,dx=np.gradient(h,spacing);length=np.sqrt(dx*dx+dz*dz+1)
  shade=np.clip((-.5*dx+.5*dz+.7071)/length,0,1)
  axs[row,col].imshow(shade,cmap='gray',extent=ext,origin='lower',vmin=0,vmax=1)
  axs[row,col].set_title(f'{kind.title()} - {key}');axs[row,col].set_xlabel('Metres');axs[row,col].set_ylabel('Metres')
 d=np.genfromtxt(p/f'weathering-{kind}-profile.csv',delimiter=',',names=True)
 ax=axs[row,2];ax.plot(d['distance']*.1,d['before']*.1,label='Before');ax.plot(d['distance']*.1,d['after']*.1,label='Rock shoulder, face and debris')
 ax.set_title('Downhill cross-section');ax.set_xlabel('Metres');ax.set_ylabel('Elevation (metres)');ax.legend();ax.grid(alpha=.2)
fig.suptitle('Actual geometry: local rock weathering at two scales');fig.tight_layout();fig.savefig(p/'weathering-comparison.png',dpi=160)
