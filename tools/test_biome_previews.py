"""Repeatable real-game biome previews. Always uses isolated saves."""
import time
import re
from test_town_combat import run

SITES = [
    ("biome-mixed-forest", "16050,0,0", "0\n220 select\n221\n250 l2\n270\n"),
    ("biome-riverbank", "11700,-4200,1.57", "0\n220 select\n221\n250 l2\n285\n"),
    ("biome-dry-hills", "-10050,28000,0", "0\n"),
    ("biome-warm-forest", "0,12300,0", "0\n"),
    ("biome-river-walk", "11700,-4200,1.57", "0\n230 128 0 128 128\n540\n"),
]

if __name__ == "__main__":
    for name, position, script in SITES:
        start = time.monotonic()
        log = run(name, "edit:0", 600 if name.endswith("walk") else 360, script,
                  {"DC_TERRAIN_TEST_SPAWN": position, "DC_TERRAIN_TRACE": "1"})
        assert "loaded retail e02t01" in log and "loaded retail e03t02" in log and "loaded retail e04t01" in log
        if name.endswith("walk"):
            positions=[tuple(map(float,row)) for row in re.findall(r"terrain: player ([-\d.]+) ([-\d.]+) ([-\d.]+) ground ([-\d.]+)",log)]
            x,y,z,h=positions[-1]
            assert abs(y-h)<1,positions[-1]
            assert (x-11700)**2+(z+4200)**2>200**2,positions[-1]
        print(f"PASS {name}: {time.monotonic()-start:.1f}s", flush=True)
