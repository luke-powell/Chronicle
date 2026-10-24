"""Reproduce the first Mayor gift cutscene using an isolated copy of a save.

Usage: python tools/test_mayor_interior.py --save-root PATH_TO_SAVE_ROOT
The supplied slot 1 must precede the first Mayor gift event.
"""
import argparse
import hashlib
import re
import shutil
from pathlib import Path

from test_town_combat import OUT, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--save-root', type=Path, required=True)
    args = parser.parse_args()
    source = args.save_root / 'saves/1/save.dat'
    before = hashlib.sha256(source.read_bytes()).hexdigest()
    name = 'mayor-cutscene-fixed'
    shutil.copytree(args.save_root, OUT / f'{name}-save', dirs_exist_ok=True)
    # Advance the gift dialogue, stop confirming before ordinary conversation,
    # then walk and leave several hundred more frames for camera updates.
    script = '0\n' + ''.join(f'{i} cross\n{i+1}\n' for i in range(300, 2200, 25))
    script += '2400 160 128 128 128\n2430\n'
    log = run(name, 'edit:0', 3100, script,
              {'DC_TEST_MAYOR_INTERIOR': '1', 'DC_TEST_LOAD_SLOT': '1'})
    assert re.findall(r'mayor regression: interior mode (\d+)', log) == ['0', '4', '0']
    for item in [236, 146, 148, 177, 175, 151]:
        assert f'GetITEM No === {item}' in log, item
    assert 'mayor regression: walking continues after item cutscene' in log
    assert hashlib.sha256(source.read_bytes()).hexdigest() == before
    print('PASS Mayor gift items, completed cutscene, resumed walking and subsequent camera updates; source save unchanged')


if __name__ == '__main__':
    main()
