"""Compare actual polygon cross-sections, including the crown, not normals."""
from pathlib import Path
import json
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT/'port/build/canyon-mesh-comparison'
OUT.mkdir(exist_ok=True)


def mesh(path, native=False):
    vertices, faces = [], []
    obj = material = ''
    for line in path.read_text().splitlines():
        t = line.split()
        if not t:
            continue
        if t[0] == 'v':
            vertices.append(list(map(float, t[1:4])))
        elif t[0] == 'o':
            obj = t[1]
        elif t[0] == 'usemtl':
            material = t[1]
        elif t[0] == 'f' and (not native or (obj.startswith('waku') and material in ('e01b05', 'e01b06'))):
            faces.append([int(x.split('/')[0])-1 for x in t[1:4]])
    return np.array(vertices)[faces]


def section(triangles, origin, direction, low, high):
    """Intersect actual triangles with a vertical plane; retain polygon edges."""
    result = []
    for tri in triangles:
        p = tri[:, [0, 2]]-origin
        d = p[:, 0]*direction[1]-p[:, 1]*direction[0]
        cuts = []
        for i in range(3):
            j = (i+1) % 3
            if (d[i] >= 0) != (d[j] >= 0):
                q = tri[i]+(tri[j]-tri[i])*d[i]/(d[i]-d[j])
                r = np.dot(q[[0, 2]]-origin, direction)
                cuts.append([r, q[1]])
        if len(cuts) == 2:
            a, b = sorted(cuts)
            if low < a[0] < high and b[0]-a[0] > .001:
                result.append([a, b])
    return sorted(result)


native = mesh(ROOT/'port/build/norune-boundary.obj', True)
generated = mesh(ROOT/'port/build/canyon-rendered.obj')
fig, ax = plt.subplots(figsize=(11, 6), constrained_layout=True)
report = {'native': [], 'generated': {}}
for angle in (0, 30, 120, 210, 315):
    a = np.deg2rad(angle)
    lines = section(native, np.array([0, 0]), np.array([np.cos(a), np.sin(a)]), 750, 1350)
    peak = max((p for line in lines for p in line), key=lambda p: p[1])
    ascending = [(p, q) for p, q in lines if p[0] < peak[0] and q[1] > p[1] and q[1] > 105]
    # Align the first upper face, keeping the native widths/heights unscaled.
    start = ascending[0][0][0]
    profile = np.array([p for line in ascending for p in line])
    ax.plot(profile[:, 0]-start, profile[:, 1], '.-', alpha=.55, label=f'Vanilla {angle}° section')
    last = ascending[-1]
    report['native'].append({'angle': angle, 'last_ascending_face_degrees': float(np.degrees(np.arctan2(last[1][1]-last[0][1], last[1][0]-last[0][0])))})

lines = section(generated, np.array([-3073.627, 1528.998]), np.array([1., 0.]), 0, 600)
upper = [(p, q) for p, q in lines if q[1] > p[1]+.001 and q[1] > 104 and p[1] < 200]
assert upper, 'Capture the renderer near -3073.627,1528.998 first'
start = upper[0][0][0]
profile = np.array([p for line in upper for p in line])
ax.plot(profile[:, 0]-start, profile[:, 1], 'ko-', linewidth=2, markersize=3, label='Generated: actual rendered polygons')
last = upper[-1]
angle = float(np.degrees(np.arctan2(last[1][1]-last[0][1], last[1][0]-last[0][0])))
report['generated'] = {'last_ascending_face_degrees': angle, 'polygon_segments': len(upper)}
assert angle < 15, report
ax.plot([0, 123], [105, 200], 'r--', label='Previous single upper face')
ax.set(xlabel='Distance from upper-wall start (world units)', ylabel='Height above floor (world units)', title='Cliff crown geometry: native polygon sections vs rendered generator')
ax.grid(alpha=.2)
ax.legend(fontsize=8)
fig.savefig(OUT/'crown-sections.png', dpi=150)
(OUT/'crown-sections.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
