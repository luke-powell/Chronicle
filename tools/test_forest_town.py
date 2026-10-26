"""Capture the integrated forest district with retail assets and an isolated save.

Run after building darkcloud. The executable is copied so previews do not lock
the build output. Images require visual inspection; logs enforce asset loading.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SHOTS = {
    'village': '-14000,-11700,-14000,200,-10000,1800',
    'river': '-14350,-10800,-14350,187,-10700,80',
    'river-reverse': '-14350,-10600,-14350,187,-10700,80',
    'treehouse': '-15650,-9963,-15823,305,-9963,35',
    'treant': '-15050,-8100,-15050,290,-7700,70',
    'waterfall': '-13150,-8900,-13150,330,-8580,100',
    'clearing': '-14300,-10750,-14300,225,-10400,45',
    'cliff': '-14000,-10650,-14000,300,-10100,85',
    'pond': '-14850,-7850,-15050,190,-7900,160',
    'outlet-reverse': '-15200,-8100,-15050,180,-8250,200',
    'outlet': '-14900,-8250,-15050,180,-8100,250',
}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shots', nargs='*', choices=tuple(SHOTS))
    parser.add_argument('--frames', type=int, default=100)
    args = parser.parse_args()
    build = ROOT / 'port/build/windows-x64-release'
    executable = build / 'forest-preview.exe'
    shutil.copy2(build / 'darkcloud.exe', executable)
    output = ROOT / 'port/run/mod-tests/forest'
    output.mkdir(parents=True, exist_ok=True)
    for name in args.shots or SHOTS:
        env = os.environ.copy()
        site={'treehouse':'-15850,-9900,0','treant':'-14900,-7900,0','outlet':'-15050,-8400,0','pond':'-14900,-7900,0','outlet-reverse':'-15050,-8250,0','clearing':'-14300,-10750,0','cliff':'-14000,-10650,0','waterfall':'-13150,-8900,0'}.get(name,'-14000,-10000,0')
        env.update(DC_WORLD_PREVIEW_SITE=site,
                   DC_WORLD_PREVIEW_GROUND_CAMERA=SHOTS[name], DC_TOWN_WALK_TRACE='1')
        log_path = output / (name + '-final.log')
        with log_path.open('w') as log:
            subprocess.run([str(executable), '--data', str(ROOT / 'port/run/data'),
                            '--save', str(output / 'preview-save'), '--offscreen',
                            '--jump', 'edit:0', '--fast-load', '--frames', str(args.frames),
                            '--width', '1280', '--height', '720', '--screenshot',
                            str(output / (name + '-final.png'))],
                           env=env, stdout=log, stderr=log, check=True, timeout=600)
        text = log_path.read_text(errors='replace')
        assert 'animated Treant ready' in text, name
        assert 'missing=0' in text, name
        assert 'forest town: missing' not in text, name
        print(f'{name}: assets loaded; inspect {output / (name + "-final.png")}', flush=True)
