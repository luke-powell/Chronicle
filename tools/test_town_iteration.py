"""Town regression fixtures. Optional --saved-layout points to a save root to COPY."""
import argparse
import hashlib
import re
import shutil
from pathlib import Path
from test_town_combat import run, OUT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--saved-layout', type=Path)
    args = parser.parse_args()
    log = run('mayor-pieces', 'edit:0', 300, '0\n', {'DC_GEORAMA_TEST': '1'})
    assert 'TEST PASS: independent mayor pieces' in log
    log = run('time-cycle', 'edit:0', 400, '0\n', {'DC_TEST_TIME_SCALE': '100'})
    periods = re.findall(r'seamless time: (\d) -> (\d) time [\d.]+ mode (\d+) day (\d+)', log)
    assert {(a, b) for a, b, _, _ in periods} == {('0', '1'), ('1', '2'), ('2', '3'), ('3', '0')}
    assert all(mode == '1' for _, _, mode, _ in periods), periods
    assert int(periods[-1][3]) >= 2
    print('PASS Mayor pieces, moved entrance, support and native save/load; seamless clock across two days', flush=True)
    if args.saved_layout:
        original = args.saved_layout / 'saves/1/save.dat'
        before = hashlib.sha256(original.read_bytes()).hexdigest()
        shutil.copytree(args.saved_layout, OUT / 'saved-layout-save', dirs_exist_ok=True)
        log = run('saved-layout', 'edit:0', 360, '0\n220 select\n221\n',
                  {'DC_TEST_LOAD_SLOT': '1', 'DC_TERRAIN_TEST_SPAWN': '37400,2000,1.57', 'DC_GEORAMA_TRACE': '1'})
        assert 'loaded placement 0 at 37600.000 1831.947 2050.000' in log
        assert 'loaded placement 1 at 37800.000 1912.072 2450.000' in log
        roads = [float(h) for h in re.findall(r'loaded placement 14 at [\d.]+ ([\d.]+)', log)]
        assert len(roads) == 7 and all(a <= b + .001 for a, b in zip(roads, roads[1:])), roads
        gaps = [float(g) for g in re.findall(r'road drape: \d+ triangles max surface error ([\d.]+)', log)]
        assert gaps and max(gaps) < .15, gaps
        assert hashlib.sha256(original.read_bytes()).hexdigest() == before
        print(f'PASS saved layout: unchanged house heights, seven continuously graded roads, max mesh error {max(gaps):.3f}', flush=True)


if __name__ == '__main__':
    main()
