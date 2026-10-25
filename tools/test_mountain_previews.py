"""Ground-level mountain evidence from the actual renderer and isolated saves.

Default captures include all vegetation, water, textures and LOD. The eye is
1.6 metres above the collision surface. --diagnostic omits flora ONLY to isolate
material problems; diagnostic images are never final acceptance evidence.
"""
import argparse
import time
from test_town_combat import run

SITES = {
    'river-bank': ('225566.96,419502.84,0', '225616.96,419502.84,225633.07,2664.61,419719.49'),
    'hill-ledge': ('8610.93,-45002.66,0', '8560.93,-45002.66,8367.01,742.22,-45168.56'),
    'dry-shoulders': ('-104625,61893,-1.5707963', '-104675,61893,-125000,14000,70312'),
    'dry-range': ('-124950,36312,0', '-125000,36312,-125000,12000,70312'),
    'wet-foothills': ('236559,426286,0', '236609,426286,252000,17000,384000'),
}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sites', nargs='*', choices=tuple(SITES))
    parser.add_argument('--diagnostic', action='store_true')
    args = parser.parse_args()
    for name in args.sites or SITES:
        site, camera = SITES[name]
        env = {'DC_WORLD_PREVIEW_SITE': site, 'DC_WORLD_PREVIEW_GROUND_CAMERA': camera,
               'DC_TERRAIN_PERF': '1'}
        if args.diagnostic:
            env['DC_TERRAIN_DIAGNOSTIC'] = 'materials'
        start = time.monotonic()
        run('mountain-' + name + ('-diagnostic' if args.diagnostic else ''),
            'edit:0', 200, '0\n', env, timeout=600)
        print(f'Captured {name} in {time.monotonic()-start:.1f}s; visual inspection required', flush=True)
