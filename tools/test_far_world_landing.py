"""Replay the far-away slot-2 landing regression using copies of the user save."""
import hashlib
import re
import shutil
from test_town_combat import ROOT, OUT, run


def main():
    source = ROOT / "port/run/town-combat-preview-save"
    save = source / "saves/2/save.dat"
    before = hashlib.sha256(save.read_bytes()).digest()
    env = {"DC_TEST_LOAD_SLOT": "2", "DC_TERRAIN_TEST_SPAWN": "-36952796,7094121.5,4.712389",
           "DC_TERRAIN_TRACE": "1", "DC_EDITOR_TRACE": "1"}
    fixtures = [
        ("far-spawn-ready", 150, "0\n", False),
        ("far-tree-landing", 800, "0\n300 select\n301\n500 select\n501\n", True),
        ("far-mountain-landing", 1800,
         "0\n220 select\n221\n280 l2\n650\n670 255 128 128 128\n940\n950 select\n951\n", True),
    ]
    for name, frames, script, landing in fixtures:
        shutil.copytree(source, OUT / (name + "-save"), dirs_exist_ok=True)
        log = run(name, "edit:0", frames, script, env)
        assert "loading teleport destination" in log, name
        if landing:
            assert "editor: walk resumed" in log, name
            tail = log.split("editor: walk resumed")[-1]
            assert "editor: mode" not in tail, name
            rows = re.findall(r"terrain surface: ([-\d.]+).*?\nterrain: player ([-\d.]+) ([-\d.]+) ([-\d.]+)", tail)
            assert rows and abs(float(rows[-1][0]) - float(rows[-1][2])) < .1, (name, rows)
            if "mountain" in name:
                assert float(rows[-1][2]) > 12000, rows[-1]
        assert hashlib.sha256(save.read_bytes()).digest() == before, "original save changed"
        print("PASS", name, flush=True)


if __name__ == "__main__":
    main()
