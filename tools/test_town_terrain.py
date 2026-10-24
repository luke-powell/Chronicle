"""Real-game terrain checks, isolated saves, offscreen screenshots and source/build hashes.

Run after Build-Chronicle.cmd. Artifacts live under ignored port/run/mod-tests/integration.
The pure kernel test is separately compiled from test_terrain_kernel.cpp (C++20).
"""
import hashlib
import json
import re
from test_town_combat import run, ROOT, OUT, EXE


def main():
    evidence = {"exe_sha256": hashlib.sha256(EXE.read_bytes()).hexdigest(),
                "sources": {}, "fixtures": {}}
    for name in ["port/src/town_terrain.cpp", "port/include/terrain_kernel.hpp",
                 "ps2/src/editground.cpp", "port/src/editloop.cpp"]:
        evidence["sources"][name] = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
    fixtures = [
        ("terrain-mayor", "0,400,3.14159", 300, "0\n", False),
        ("terrain-platform", "180,700,0", 360, "0\n", False),
        ("terrain-boundary", "0,-680,0", 720, "0\n240 128 0 128 128\n650\n", True),
        ("terrain-hills", "2800,0,0", 720, "0\n240 128 0 128 128\n650\n", True),
        ("terrain-far", "10000000,-20000000,0", 360, "0\n", True),
    ]
    for name, spawn, frames, script, terrain_ground in fixtures:
        log = run(name, "edit:0", frames, script,
                  {"DC_TERRAIN_TRACE": "1", "DC_TERRAIN_TEST_SPAWN": spawn})
        assert "preserved Mayor platform, 73 triangles" in log
        chunks = [int(n) for n in re.findall(r"chunks (\d+)", log)]
        assert chunks and max(chunks) < 2000
        assert "radius 112640000 levels 0..16" in log
        positions = [tuple(map(float, row)) for row in re.findall(
            r"terrain: player ([-\d.]+) ([-\d.]+) ([-\d.]+) ground ([-\d.]+) far ([-\d.]+)", log)]
        assert positions and positions[-1][4] == 225280000
        x, y, z, ground, _ = positions[-1]
        if terrain_ground:
            assert abs(y-ground) < 0.2, (name, y, ground)
        if name == "terrain-platform":
            assert 40 < y < 55, ("Mayor's platform collision missing", y)
        if name == "terrain-boundary":
            assert z < -1200, ("old cliff still blocks movement", z)
        if name == "terrain-hills":
            assert z < -600, ("did not cross chunk boundaries", z)
        evidence["fixtures"][name] = {"max_chunks": max(chunks), "positions": positions}
        print(f"PASS {name}: position {x:.1f}, {y:.1f}, {z:.1f}; max {max(chunks)} chunks", flush=True)
    (OUT / "terrain-evidence.json").write_text(json.dumps(evidence, indent=2))


if __name__ == "__main__":
    main()
