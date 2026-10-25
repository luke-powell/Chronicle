"""Compare exported native and resident canyon wall triangulation (no game needed).

Inputs: DC_TOWN_BOUNDARY_OBJ and DC_TERRAIN_MESH_OBJ exports in port/build.
The generated sample excludes the authored town transition (radius < 1950).
Report world-unit measurements rather than assuming a physical metre scale.
"""
from pathlib import Path
import json
import sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'port/build/canyon-mesh-comparison'
OUT.mkdir(exist_ok=True)


def read_mesh(path, native):
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
        elif t[0] == 'f':
            if native and (not obj.startswith('waku') or material not in ('e01b05', 'e01b06')):
                continue
            ids = [int(s.split('/')[0])-1 for s in t[1:]]
            for i in range(1, len(ids)-1):
                faces.append([ids[0], ids[i], ids[i+1]])
    p = np.asarray(vertices)[np.asarray(faces)]
    if not native:
        centre = p.mean(axis=1)
        p = p[(np.max(np.abs(centre[:, [0, 2]]), axis=1) > 3072)
              & (centre[:, 1] > 0) & (centre[:, 1] < 200)]
    cross = np.cross(p[:, 1]-p[:, 0], p[:, 2]-p[:, 0])
    area = np.linalg.norm(cross, axis=1)*.5
    valid = area > 1e-6
    p, cross, area = p[valid], cross[valid], area[valid]
    normal = cross/(2*area[:, None])
    slope = np.degrees(np.arccos(np.clip(abs(normal[:, 1]), 0, 1)))
    edges = np.linalg.norm(p-np.roll(p, -1, axis=1), axis=2)
    # Equilateral=1; long thin or degenerate triangles approach zero.
    quality = 4*np.sqrt(3)*area/np.sum(edges**2, axis=1)
    return dict(p=p, area=area, normal=normal, slope=slope, edges=edges, quality=quality)


def quantiles(a):
    return dict(zip(('p10', 'median', 'p90'), map(float, np.percentile(a, [10, 50, 90]))))


def stats(mesh, mask):
    p, normals = mesh['p'][mask], mesh['normal'][mask]
    # Weld coincident positions: native OBJ duplicates vertices at UV seams.
    _, ids = np.unique(np.round(p.reshape(-1, 3), 3), axis=0, return_inverse=True)
    ids = ids.reshape(-1, 3)
    adjacency = {}
    for face, tri in enumerate(ids):
        for a, b in ((0, 1), (1, 2), (2, 0)):
            key = tuple(sorted((int(tri[a]), int(tri[b]))))
            adjacency.setdefault(key, []).append(face)
    angles = []
    for shared in adjacency.values():
        if len(shared) == 2:
            angles.append(np.degrees(np.arccos(np.clip(np.dot(normals[shared[0]], normals[shared[1]]), -1, 1))))
    area = mesh['area'][mask]
    return {
        'triangles': int(mask.sum()), 'surface_area_world_units_squared': float(area.sum()),
        'triangles_per_10000_square_world_units': float(len(area)/area.sum()*10000),
        'triangle_area': quantiles(area), 'edge_length': quantiles(mesh['edges'][mask].ravel()),
        'triangle_quality_equilateral_is_one': quantiles(mesh['quality'][mask]),
        'adjacent_face_angle_degrees': quantiles(angles),
        'area_weighted_mean_slope_degrees': float(np.average(mesh['slope'][mask], weights=area)),
    }


meshes = {'Native Norune': read_mesh(ROOT/'port/build/norune-boundary.obj', True),
          'Generated canyon': read_mesh(ROOT/'port/build/canyon-rendered.obj', False)}
report = {'method': 'Native waku e01b05/e01b06 versus resident LOD0 outside the native collar and its adjoining grid (max |x,z| > 3072); slopes 10–60 and 60–90 degrees. Samples are different surface extents; compare density and distributions, not total triangle counts.', 'samples': {}}
fig, axes = plt.subplots(2, 3, figsize=(13, 7), constrained_layout=True)
for row, (band, lo, hi) in enumerate((('Upper slopes', 10, 60), ('Steep walls', 60, 90.001))):
    report['samples'][band] = {}
    for label, mesh in meshes.items():
        mask = (mesh['slope'] >= lo) & (mesh['slope'] < hi)
        report['samples'][band][label] = stats(mesh, mask)
        for col, (field, title) in enumerate((('area', 'Triangle area (world units²)'), ('edges', 'Edge length (world units)'), ('quality', 'Triangle quality (1 = equilateral)'))):
            values = np.sort(mesh[field][mask].ravel())
            axes[row, col].plot(values, np.arange(1, len(values)+1)/len(values), label=label)
            if field != 'quality':
                axes[row, col].set_xscale('log')
            axes[row, col].set_title(f'{band}: {title}')
            axes[row, col].set_ylabel('Fraction of triangles / edges')
            axes[row, col].grid(alpha=.25)
    axes[row, 0].legend()
fig.suptitle('Actual rendered wall meshes — native Norune vs procedural canyon')
fig.savefig(OUT/'distributions.png', dpi=150)
(OUT/'metrics.json').write_text(json.dumps(report, indent=2))
print(json.dumps(report, indent=2))
if '--check' in sys.argv:
    for band, samples in report['samples'].items():
        native, generated = samples['Native Norune'], samples['Generated canyon']
        # Allow contour intersections / the town transition, but reject the
        # old 24–88x density and stretched steep-wall triangle distribution.
        ratio = generated['triangles_per_10000_square_world_units']/native['triangles_per_10000_square_world_units']
        # Three crown bands plus relief intersections replace the former single
        # upper plane. Keep that bounded at 3.25x native density (formerly 88x);
        # the steep wall retains its original 2x budget.
        assert ratio < (3.25 if band == 'Upper slopes' else 2), (band, 'excessive density', ratio)
        assert generated['triangle_quality_equilateral_is_one']['median'] > .6, (band, 'thin triangles')
        edge_ratio=generated['edge_length']['median']/native['edge_length']['median']
        assert .5 < edge_ratio < 1.5, (band, 'wrong polygon scale', edge_ratio)
        assert generated['adjacent_face_angle_degrees']['p90'] < 45, (band, 'jagged faces')
        assert abs(native['area_weighted_mean_slope_degrees']-generated['area_weighted_mean_slope_degrees']) < 12, (band, 'wrong slope')
    print('PASS native-relative density, triangle shape, face angles and wall slopes')
