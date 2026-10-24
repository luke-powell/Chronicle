"""Real-game regression for shared origin, physics, camera, and precise saves."""
import hashlib
import json
import re
import shutil
from test_town_combat import ROOT, OUT, run

SOURCE = ROOT / "port/run/town-combat-preview-save"


def replay(name, spawn, script="0\n", frames=440, extra=None, source=SOURCE):
    shutil.copytree(source, OUT / (name + "-save"), dirs_exist_ok=True)
    return run(name, "edit:0", frames, script, {
        "DC_TEST_LOAD_SLOT": "2", "DC_TERRAIN_TEST_SPAWN": spawn,
        "DC_WORLD_PRECISION_TRACE": "1", "DC_TERRAIN_TRACE": "1", **(extra or {})},
        extra_args=["--display-per-tick", "2"])


def camera_rows(log):
    origin = (0, 0)
    rows = []
    for line in log.splitlines():
        if line.startswith("world origin:"):
            origin = tuple(map(float, line.split(":")[1].split()))
        match = re.match(r"precision camera: tick (\d+) player ([-\d.e+]+) ([-\d.e+]+) eye ([-\d.e+]+) ([-\d.e+]+) ref ([-\d.e+]+) ([-\d.e+]+)", line)
        if match:
            t,x,z,ex,ez,rx,rz = map(float, match.groups())
            rows.append((t,x+origin[0],z+origin[1],ex+origin[0],ez+origin[1]))
    return rows


def main():
    original = SOURCE / "saves/2/save.dat"
    before = hashlib.sha256(original.read_bytes()).digest()
    walk = "0\n260 255 128 128 128\n400\n"
    log = replay("world-threshold", "33554420,0,0", walk)
    assert max(row[1] for row in camera_rows(log)) > 33554434
    print("PASS cross old 2^25 movement barrier", flush=True)
    log = replay("world-continuous-rebase", "-36952796,7094121.5,0",
                 "0\n260 128 0 128 128\n2800\n", frames=3000)
    assert log.count("world origin:") >= 2
    rows = [r for r in camera_rows(log) if 260 <= r[0] <= 2800]
    assert max(abs(b[1]-a[1]) + abs(b[2]-a[2]) for a,b in zip(rows,rows[1:])) < 3
    print("PASS walking through origin shift without world-position jump", flush=True)
    log = replay("world-user-walk", "-36952796,7094121.5,0", walk)
    steps = re.findall(r"precision local move:.*?requested ([-\d.e+]+) ([-\d.e+]+) stored_delta ([-\d.e+]+) ([-\d.e+]+)", log)
    assert len(steps) >= 100
    for requested_x,requested_z,actual_x,actual_z in steps[:10]:
        assert abs(float(requested_x)-float(actual_x)) < .001
        assert abs(float(requested_z)-float(actual_z)) < .001
    print("PASS user-position movement retains sub-unit precision", flush=True)
    log = replay("world-user-orbit", "-36952796,7094121.5,0", "0\n260 128 128 255 128\n400\n")
    rows = [r for r in camera_rows(log) if 280 <= r[0] < 400]
    dx = [b[3]-a[3] for a,b in zip(rows,rows[1:])]
    assert dx.count(0) == 0 and max(map(abs,dx)) < 3
    assert "precision collider:" not in log
    print("PASS smooth camera orbit; no collider edge errors above .05", flush=True)
    log = replay("world-extreme", "100000000,-100000000,0", walk)
    rows = [r for r in camera_rows(log) if r[0] >= 260]
    assert max(r[1] for r in rows)-min(r[1] for r in rows) > 20
    print("PASS movement at 100 million units on both axes", flush=True)
    log = replay("world-rebase-stability", "-36952796,7094121.5,4.712389", frames=300,
                 extra={"DC_WORLD_REBASE_TEST":"1"})
    assert "40 origin shifts preserve exact double positions" in log
    log = replay("world-save", "-36952796,7094121.5,4.712389", frames=300,
                 extra={"DC_WORLD_SAVE_TEST":"1"})
    assert "wrote fractional far placement" in log
    saved_root = OUT / "world-save-save"
    data = json.loads((saved_root / "saves/autosave/world.json").read_text())
    tree = next(p for p in data["parts"] if p["plot"] == 13)
    assert tree["x"] == -36952751.875 and tree["z"] == 7094150.25
    log = replay("world-load", "-36952796,7094121.5,4.712389", frames=300,
                 extra={"DC_TEST_LOAD_SLOT":"-1", "DC_WORLD_RELOAD_TEST":"1"}, source=saved_root)
    assert "world load placement: 13 -36952751.875" in log
    assert "native save reloaded with nonzero origin" in log
    assert log.count("world load placement: 13 -36952751.875") >= 2
    assert hashlib.sha256(original.read_bytes()).digest() == before
    print("PASS exact origin round trips, fractional native save/load and live reload; original save unchanged", flush=True)


if __name__ == "__main__":
    main()
