"""Plot the actual C++ generator's exported fields (no separate noise simulation).

Run preview_biomes 0 0 180000 400 <fields.csv>, then this script <fields.csv>.
"""
import sys
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

source = Path(sys.argv[1])
data = np.genfromtxt(source, delimiter=",", names=True)
n = int(np.sqrt(len(data)))
assert n*n == len(data)
fig, axes = plt.subplots(2, 3, figsize=(15, 10), constrained_layout=True)
fields = [("height", "Elevation", "terrain", None),
          ("rainfall", "Rainfall + mountain rain shadow", "Blues", (0, 1)),
          ("temperature", "Temperature + altitude cooling", "coolwarm", (0, 1)),
          ("moisture", "Available soil + riverbank moisture", "YlGnBu", (0, 1)),
          ("forest", "Continuous forest suitability", "YlGn", (0, 1)),
          ("sand", "Continuous dryland suitability", "YlOrBr", (0, 1))]
for ax, (field, label, cmap, limits) in zip(axes.flat, fields):
    image = ax.imshow(data[field].reshape(n,n), cmap=cmap,
                      vmin=limits[0] if limits else None,
                      vmax=limits[1] if limits else None,
                      extent=(data['x'].min()/1000, data['x'].max()/1000,
                              data['z'].max()/1000, data['z'].min()/1000))
    ax.set_title(label)
    ax.set_xlabel("World X / 1,000 game units")
    ax.set_ylabel("World Z / 1,000 game units")
    fig.colorbar(image, ax=ax, shrink=.75)
fig.suptitle("Shared ecology fields — actual terrain generator", fontsize=18)
target = source.with_suffix('.png')
fig.savefig(target, dpi=140)
print(target)
