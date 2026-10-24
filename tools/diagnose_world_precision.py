"""Measure large-world failures in real game ticks without changing player saves.

Requires extracted assets and town-combat-preview-save slot 2. This is a
diagnostic, not a test that requires the engine to remain broken.
"""
import hashlib
import json
import re
import shutil
from test_town_combat import ROOT, OUT, EXE, run


def main():
    source = ROOT / "port/run/town-combat-preview-save"
    save = source / "saves/2/save.dat"
    before = hashlib.sha256(save.read_bytes()).hexdigest()
    evidence = {"exe_sha256": hashlib.sha256(EXE.read_bytes()).hexdigest(),
                "save_sha256": before, "fixtures": {}}
    fixtures = [
        ("precision-user-x", "-36952796,7094121.5,0", "255 128 128 128"),
        ("precision-user-z", "-36952796,7094121.5,0", "128 0 128 128"),
        ("precision-boundary", "33554420,0,0", "255 128 128 128"),
        ("precision-mid", "16777220,0,0", "255 128 128 128"),
        ("precision-near", "100100,0,0", "255 128 128 128"),
        ("precision-orbit-far", "-36952796,7094121.5,0", "128 128 255 128"),
        ("precision-orbit-near", "100,0,0", "128 128 255 128"),
    ]
    for name, spawn, stick in fixtures:
        shutil.copytree(source, OUT / (name + "-save"), dirs_exist_ok=True)
        log = run(name, "edit:0", 440, f"0\n260 {stick}\n400\n", {
            "DC_TEST_LOAD_SLOT": "2", "DC_TERRAIN_TEST_SPAWN": spawn,
            "DC_WORLD_PRECISION_TRACE": "1"})
        moves = [line for line in log.splitlines() if line.startswith("precision move:")]
        errors = [float(x) for x in re.findall(r"max_edge_error ([\d.e+-]+)", log)]
        cameras = [list(map(float, row)) for row in re.findall(
            r"precision camera: tick (\d+) player ([-\d.e+]+) ([-\d.e+]+) eye ([-\d.e+]+) ([-\d.e+]+) ref ([-\d.e+]+) ([-\d.e+]+)", log)]
        cameras = [row for row in cameras if 280 <= row[0] < 400]
        steps = [b[3] - a[3] for a, b in zip(cameras, cameras[1:])]
        result = {"first_moves": moves[:3], "last_moves": moves[-3:],
                  "movement_ticks": len(moves), "camera_x_still_ticks": steps.count(0),
                  "camera_sample_count": len(steps), "camera_x_steps": sorted(set(steps)),
                  "max_sampled_collider_edge_error": max(errors, default=0),
                  "collider_note": "First 100 reports with error > .05; zero means none reported, not exact zero error."}
        evidence["fixtures"][name] = result
        assert hashlib.sha256(save.read_bytes()).hexdigest() == before, "Player save changed"
        print(name, json.dumps(result), flush=True)
    (OUT / "precision-audit.json").write_text(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
