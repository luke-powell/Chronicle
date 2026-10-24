"""Exercise the real native save/sidecar round trip with an isolated save."""
import json
import re
import shutil
from test_town_combat import run, OUT

name = "player-position-save"
log = run(name, "edit:0", 245, "0\n", {
    "DC_TERRAIN_TEST_SPAWN": "16050,0,0",
    "DC_WORLD_SAVE_TEST": "1",
}, timeout=180)
source = OUT / (name + "-save") / "saves" / "autosave"
player = json.loads((source / "world.json").read_text())["player"]
assert player["town"] == 0 and abs(player["x"]) > 10000, player
name = "player-position-load"
dest = OUT / (name + "-save") / "saves" / "autosave"
shutil.copytree(source, dest, dirs_exist_ok=True)
log = run(name, "edit:0", 245, "0\n", {
    "DC_TEST_LOAD_SLOT": "-1",
    "DC_WORLD_SAVE_TEST": "1",
}, timeout=180)
restored = re.findall(r"world player restored: ([^ ]+) ([^ ]+) ([^ ]+) yaw ([^\n]+)", log)
assert restored, "saved player was not restored"
for x, y, z, yaw in restored:
    assert abs(float(x)-player["x"]) < 1e-8
    assert abs(float(z)-player["z"]) < 1e-8
    assert abs(float(yaw)-player["yaw"]) < 1e-6
saved_again = json.loads((dest / "world.json").read_text())["player"]
assert abs(saved_again["x"]-player["x"]) < .1, (player, saved_again)
assert abs(saved_again["z"]-player["z"]) < .1, (player, saved_again)
print("PASS native save, double position/facing restore, arrival-event overwrite protection")
