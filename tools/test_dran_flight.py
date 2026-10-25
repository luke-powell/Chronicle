"""Exercise Dran with real game assets and isolated saves; never open a player save.

python tools/test_dran_flight.py [lifecycle|travel|dungeon|combat|camera|performance]
Requires port/run/data and the Windows release build. Logs and PNGs are retained.
"""
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "port/run/mod-tests/dran"


def run(name, frames, script, extra=None, mode="edit:0"):
    OUT.mkdir(parents=True, exist_ok=True)
    inputs = OUT / f"{name}.input"
    inputs.write_text(script, encoding="ascii")
    log = OUT / f"{name}.log"
    screenshot = OUT / f"{name}.png"
    with log.open("w", encoding="utf-8") as stream:
        result = subprocess.run([
            os.environ.get("DC_TEST_EXE",str(ROOT / "port/build/windows-x64-release/darkcloud.exe")),
            "--data", str(ROOT / "port/run/data"), "--save", str(OUT / f"{name}-save"),
            "--offscreen", "--jump", mode, "--fast-load", "--frames", str(frames),
            "--input", str(inputs), "--screenshot", str(screenshot),
            "--width", "960", "--height", "540",
        ], cwd=ROOT / "port/run", stdout=stream, stderr=stream, timeout=360,
            env={**os.environ, "DC_DRAN_TRACE": "1", "DC_TOWN_COMBAT_TRACE": "1", **(extra or {})})
    text = log.read_text(encoding="utf-8", errors="replace")
    assert result.returncode == 0, (name, result.returncode, log)
    assert screenshot.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"), name
    for error in ("Alocation Error", "File open error", "not found", "model load failed"):
        assert error not in text, (name, error, log)
    if "dran: takeoff" in text:
        attachments = re.findall(r"dran: attachment socket=to_c error=([\d.]+) bank=([-\d.]+)", text)
        assert attachments, "missing animated rider attachment checks"
        assert max(float(row[0]) for row in attachments) < .001, attachments
    return text


def lifecycle():
    text = run("lifecycle", 460,
        "0\n180 r3\n181\n210 cross\n220\n250 r3\n251\n330 cross\n331\n390 r3\n391\n",
        {"DC_TERRAIN_TEST_SPAWN": "0,0,0", "DC_TERRAIN_TRACE": "1"})
    assert text.count("dran: takeoff") == 2, text[-4000:]
    assert "dran: landing requested" in text
    landing = re.search(r"dran: landed ([-\d.]+) ([-\d.]+) ([-\d.]+)", text)
    assert landing, text[-4000:]
    assert abs(float(landing[2])) < .1, landing.groups()
    assert "terrain: landing ready" in text
    # Combat must resume after dismount, and be absent between takeoff and landing.
    flight = text.split("dran: takeoff")[1].split("dran: landed")[0]
    assert "town combat: motion" not in flight
    assert "town combat: motion 1000" in text.split("dran: landed")[1]


def travel():
    text = run("travel", 380,
        "0\n100 r3\n101\n130 r1 128 0 128 128\n280 r1 200 0 128 128\n320\n",
        {"DC_WORLD_PRECISION_TRACE": "1"})
    assert "dran: takeoff" in text
    rows = re.findall(r"dran: flight x=([-\d.]+) y=([-\d.]+) z=([-\d.]+) speed=([-\d.]+) floor=([-\d.]+)", text)
    assert len(rows) >= 6, rows
    assert max(float(r[3]) for r in rows) >= 79, rows
    assert all(float(r[1]) + .1 >= float(r[4]) for r in rows), rows
    assert "dran: landed" not in text
    assert "world origin:" in text, "travel did not exercise origin rebasing"


def dungeon():
    text = run("dungeon", 150, "0\n80 r3\n81\n", mode="dungeon:0")
    assert "dran: takeoff" not in text


def combat():
    text = run("combat", 160, "0\n80 cross\n81\n")
    assert "town combat: motion 1000" in text
    assert "dran: takeoff" not in text


def camera():
    text = run("camera", 1050,
        "0\n100 r3\n101 cross\n150 cross 128 128 128 0\n270 cross 128 128 128 255\n"
        "420 cross 128 128 128 0\n470\n480 cross 200 128 255 128\n530\n600 128 0 128 128\n",
        {"DC_TERRAIN_TEST_SPAWN": "0,0,0","DC_TEST_SKIP_TERRAIN_DRAW":"1"})
    rows = re.findall(r"dran: camera yaw=([-\d.]+) pitch=([-\d.]+) distance=([-\d.]+) height=([-\d.]+)", text)
    assert rows, text[-4000:]
    pitches = [float(row[1]) for row in rows]
    assert min(pitches) < -1.55 and max(pitches) > 1.55, pitches
    assert all(abs(float(r[2]) ** 2 + float(r[3]) ** 2 - 276.8122 ** 2) < 1 for r in rows), rows
    absolute=re.findall(r"worldyaw=([-\d.]+) tick=(\d+)",text)
    hover=[float(yaw) for yaw,tick in absolute if 450<=int(tick)<=480]
    assert len(hover)==2 and abs(hover[0]-hover[1])<.0002,hover
    assert abs(float(rows[-1][0]))<.02,"moving camera did not recenter"
    assert .06<float(rows[-1][1])<.28,"vertical auto-follow pulled view too far down"
    banks = re.findall(r"dran: attachment socket=to_c error=[\d.]+ bank=([-\d.]+)", text)
    assert any(abs(float(b)) > .05 for b in banks), banks


def performance():
    text = run("performance", 750,
        "0\n180 r3\n181 cross\n230 r1 128 0 128 128\n700\n",
        {"DC_TERRAIN_PERF": "1"})
    flight = text.split("dran: takeoff", 1)[1]
    assert "initial 1" not in flight, "flight must not trigger synchronous terrain reloads"
    lods = re.findall(r"lod (\d+) initial", flight)
    assert lods and min(map(int, lods)) >= 2, lods
    times = sorted(map(float, re.findall(r"town tick: ([\d.]+) ms", flight)))
    assert times, "missing flight timings"
    print(f"flight tick median={times[len(times)//2]:.3f} p99={times[int(len(times)*.99)]:.3f} max={times[-1]:.3f} ms")


if __name__ == "__main__":
    tests = {"lifecycle": lifecycle, "travel": travel, "dungeon": dungeon, "combat": combat, "camera": camera, "performance": performance}
    for name in sys.argv[1:] or tests:
        tests[name]()
        print("PASS", name, flush=True)
