"""Regression: edits retain distant coverage and chunk builds do not block frames."""
import re
from test_town_combat import run

log = run("terrain-local-edit", "edit:0", 500,
          "0\n240 128 128 255 128\n340\n380 select\n381\n400 l2\n460\n",
          {"DC_TERRAIN_PERF": "1", "DC_GEORAMA_TEST": "1", "DC_FOREST_TRACE": "1"},
          timeout=180)
assert "georama TEST PASS" in log
edits = [(int(a), int(b)) for a,b in re.findall(r"invalidated (\d+) of (\d+) cached",log) if int(b)]
assert edits and all(0 < a < b/4 for a,b in edits), edits
updates = [(float(ms), int(initial)) for ms,initial in re.findall(r"update ([0-9.]+) ms.*initial (\d+)",log)]
assert sum(initial for _,initial in updates)==1, "placement rebuilt the whole world"
regular = sorted(ms for ms,initial in updates if not initial)
assert regular[int(.99*(len(regular)-1))] < 10, regular[-10:]
ready = re.findall(r"terrain vegetation ready: level (\d+) canopy (\d+) forestMask (\d+)",log)
assert ready and all(int(canopy)==1 for _,canopy,_ in ready)
assert max(int(level) for level,_,_ in ready)>10
assert log.index("terrain vegetation ready:") < log.index("terrain draw:")
print("PASS local invalidation, retained horizon/forest coverage, budgeted render-thread uploads", edits)
