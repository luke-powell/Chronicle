"""Controller regression for the direct town editor; uses isolated scratch saves."""
import re
from test_town_combat import run


def main():
    env = {"DC_EDITOR_TRACE": "1", "DC_TERRAIN_TEST_SPAWN": "0,792,0"}
    log = run("editor-controls", "edit:0", 700,
              "0\n220 select\n221\n300 l2 255 128 128 128\n420\n450 r2\n570\n600 triangle\n601\n", env)
    samples = [tuple(map(float, row)) for row in re.findall(
        r"editor: mode (\d+) part (-?\d+) zoom ([\d.]+) cursor ([-\d.]+) ([-\d.]+)", log)]
    assert samples[0][0:2] == (3, -1), samples
    assert max(s[2] for s in samples) > 19000
    assert abs(samples[-1][2] - 1000) < .1
    assert samples[5][3] - samples[4][3] > 2 * (samples[3][3] - samples[2][3])
    log = run("editor-list-place", "edit:0", 1320,
              "0\n220 select\n221\n280 square\n281\n310 triangle\n311\n400 cross\n401\n480 down\n950\n980 up\n981\n1020 cross\n1021\n1150 down\n1151\n1200 cross\n1201\n", env)
    assert "editor board: index 17 of 19 part 22" in log
    assert "editor board: index 18 of 19 part 23" in log
    assert "editor: mode 0 part 22" in log
    assert "georama: placed plot 22 slot 1 at 50.0 47.4 800.0" in log
    assert "editor: mode 3 part -1" in log.split("at 50.0 47.4 800.0")[-1]
    log = run("editor-pickup-cancel", "edit:0", 420,
              "0\n220 select\n221\n280 cross\n281\n340 circle\n341\n", env)
    assert "editor: mode 1 part 22" in log
    assert "editor: mode 3 part -1" in log.split("editor: mode 1 part 22")[-1]
    print("PASS immediate pickup/delete, cancel, both zoom directions, scaled movement, full inventory and Mayor redeployment")


if __name__ == "__main__":
    main()
