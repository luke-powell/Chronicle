"""Offscreen zoom/pan benchmark with bounded-cache and return-to-walk checks."""
import json
import re
import statistics
from test_town_combat import run, OUT


def main():
    log = run("editor-zoom-return", "edit:0", 1800,
              "0\n220 select\n221\n280 l2\n650\n670 255 128 128 128\n940\n950 r2\n1295\n1550 select\n1551\n",
              {"DC_TERRAIN_PERF": "1", "DC_EDITOR_TRACE": "1"})
    rows = re.findall(r"update ([\d.]+) ms built (\d+) chunks (\d+) cached (\d+) pending (\d+) lod (\d+) initial (\d+)", log)
    assert rows and max(int(r[3]) for r in rows) <= 4096
    assert max(int(r[5]) for r in rows) >= 9
    assert rows[-1][4:6] == ("0", "0"), rows[-1]
    assert "zoom 5000000.0" in log
    times = sorted(float(r[0]) for r in rows if r[6] == "0")
    result = {"median_ms": statistics.median(times), "p95_ms": times[int(len(times)*.95)],
              "max_ms": max(times), "cache_limit": 4096,
              "note": "Terrain CPU updates only; initial loading excluded; not whole-frame/GPU timing."}
    (OUT / "editor-performance.json").write_text(json.dumps(result, indent=2))
    print(result)
    print("PASS 25x zoom, panning, bounded cache, settled full-detail return")


if __name__ == "__main__":
    main()
