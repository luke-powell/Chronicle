"""Real-door regression using an isolated copy of a save with a placed house.
Usage: python tools/test_town_interactions.py SAVE_ROOT [--plot 0] [--slot 1]
"""
import argparse
import re
import shutil
from pathlib import Path
from test_town_combat import run, OUT

p = argparse.ArgumentParser()
p.add_argument('save_root', type=Path)
p.add_argument('--plot', type=int, default=0)
p.add_argument('--slot', type=int, default=1)
a = p.parse_args()
for case, script, rebase in [
    ('door-rebase', '0\n260 cross\n261\n', True),
    ('door-combo', '0\n115 cross\n116\n180 cross\n181\n', False),
]:
    dest = OUT / (case + '-save')
    shutil.copytree(a.save_root, dest, dirs_exist_ok=True)
    env = {'DC_TEST_LOAD_SLOT': str(a.slot), 'DC_TEST_TOWN_DOOR': str(a.plot),
           'DC_TEST_SKIP_TERRAIN_DRAW': '1'}
    if rebase:
        env['DC_WORLD_REBASE_TEST'] = '1'
    log = run(case, 'edit:0', 430, script, env, timeout=120)
    assert 'door test: entering ' in log
    assert 'mes = gedit/' in log, 'interior did not finish loading'
    assert 'town combat: motion ' not in log.split('door test: placed')[1]
    if rebase:
        assert 'building-local interactions' in log
    else:
        assert 'town combat: motion 1000' in log, 'attack interruption was not exercised'
log = run('door-combat-regression', 'edit:0', 280,
          '0\n80 cross\n81\n140 128 0 128 128\n170 cross 128 0 128 128\n171 128 0 128 128\n240\n',
          {'DC_TEST_SKIP_TERRAIN_DRAW': '1'})
swings = log.split('town combat: motion 1000\n')[1:]
assert len(swings) == 2
assert all(len(re.findall(r'tick motion=1000 age=(\d+)', s)) >= 30 for s in swings)
print('PASS relocated doors, origin shifts, interaction priority, music-transition load and normal combat')
