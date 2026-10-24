"""Compare actual kernel exports, with matching axes and illumination."""
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.colors import LightSource

folder = Path(__file__).resolve().parents[1] / 'port/run/mod-tests/integration'
fig, axes = plt.subplots(2, 3, figsize=(17, 10), constrained_layout=True)
for row, name in enumerate(['before', 'after']):
    d = np.genfromtxt(folder / f'mountains-{name}.csv', delimiter=',', names=True)
    n = int(np.sqrt(len(d)))
    h = d['height'].reshape(n, n)
    extent = (d['x'].min()/1000, d['x'].max()/1000, d['z'].max()/1000, d['z'].min()/1000)
    spacing = (d['x'].max()-d['x'].min())/(n-1)
    light = LightSource(315, 40)
    shade = light.shade(h, cmap=plt.get_cmap('gist_earth'), vert_exag=1,
                        dx=spacing, dy=spacing, vmin=0, vmax=27000)
    axes[row, 0].imshow(shade, extent=extent)
    axes[row, 0].set_title(name.title()+' - elevation and landforms')
    im = axes[row, 1].imshow(d['slope'].reshape(n, n), vmin=0, vmax=60, cmap='magma', extent=extent)
    axes[row, 1].set_title('Slope (degrees)')
    fig.colorbar(im, ax=axes[row, 1], shrink=.7)
    rock, snow, forest = [d[k].reshape(n, n, 1) for k in ['rock', 'snow', 'forest']]
    color = np.array([.52, .58, .3])*(1-forest)+np.array([.18, .31, .17])*forest
    color = color*(1-rock)+np.array([.46, .45, .43])*rock
    color = color*(1-snow)+np.array([.92, .95, .97])*snow
    illumination = light.hillshade(h, dx=spacing, dy=spacing)[:, :, None]
    axes[row, 2].imshow(np.clip(color*(.55+.6*illumination), 0, 1), extent=extent)
    axes[row, 2].set_title('Ecology + rock / snow suitability')
    upland = d['height'] > 2000
    print(name, 'peak', d['height'].max(), 'upland gentle %', np.mean(d['slope'][upland] < 15)*100,
          'cliff %', np.mean(d['slope'][upland] > 40)*100, 'soil-capable %', np.mean(d['rock'][upland] < .4)*100)
for ax in axes.flat:
    ax.set_xlabel('World X / 1,000 game units')
    ax.set_ylabel('World Z / 1,000 game units')
fig.savefig(folder / 'mountain-comparison.png', dpi=120)
