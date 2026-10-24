"""Run the town-combat mod against real extracted game data in an isolated save.

Usage: python tools/test_town_combat.py
Requires the Windows release build and port/run/data. Screenshots and logs are
written to ignored port/run/mod-tests/integration. No player saves are opened.
"""
import os
import json
import re
import time
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "port/build/windows-x64-release/darkcloud.exe"
OUT = ROOT / "port/run/mod-tests/integration"


def run(name, mode, frames, script, extra_env=None, extra_args=None):
    OUT.mkdir(parents=True, exist_ok=True)
    input_file = OUT / f"{name}.input"
    input_file.write_text(script, encoding="ascii")
    image = OUT / f"{name}.png"
    result = subprocess.run(
        [str(EXE), "--data", str(ROOT / "port/run/data"),
         "--save", str(OUT / f"{name}-save"), "--offscreen",
         "--jump", mode, "--fast-load", "--frames", str(frames),
         "--input", str(input_file), "--width", "1280", "--height", "720",
         "--screenshot", str(image), *(extra_args or [])],
        cwd=ROOT / "port/run", capture_output=True, timeout=60,
        env={**os.environ, "DC_TOWN_COMBAT_TRACE": "1", **(extra_env or {})},
    )
    log = (result.stdout + result.stderr).decode("utf-8", errors="replace").replace("\r\n", "\n")
    (OUT / f"{name}.log").write_text(log, encoding="utf-8")
    assert result.returncode == 0, f"{name}: exit {result.returncode}; see {OUT / (name + '.log')}"
    assert image.read_bytes().startswith(b"\x89PNG\r\n\x1a\n"), name
    assert "Alocation Error" not in log and "File open error" not in log, name
    return log


def main():
    log = run("combo-five", "edit:0", 360, """0
80 cross
81
90 cross
91
120 cross
121
145 cross
146
175 cross
176
""")
    motions = [line.rsplit(" ", 1)[-1] for line in log.splitlines()
               if line.startswith("town combat: motion ")]
    assert motions == ["1000", "1001", "1002", "1003", "1004"], motions
    assert log.count("town combat: sound") == 5
    # User reproduction: finish an attack, move, then attack again while holding move.
    log = run("repeat-moving", "edit:0", 280, """0
80 cross
81
140 128 0 128 128
170 cross 128 0 128 128
171 128 0 128 128
240
""")
    swings = log.split("town combat: motion 1000\n")[1:]
    assert len(swings) == 2
    for swing in swings:
        ticks = re.findall(r"tick motion=1000 age=(\d+)", swing)
        assert len(ticks) >= 30, f"attack cut short after {len(ticks)} ticks"
    assert log.count("town combat: sound 400 shout 420") == 2
    no_jump = run("circle-no-jump", "edit:0", 140, "0\n80 circle\n81\n")
    assert "town combat: jump velocity" not in no_jump
    log = run("jump-height", "edit:0", 320, "0\n80 cross\n180\n")
    height = float(re.search(r"jump height ([\d.]+)", log)[1])
    assert 19 <= height <= 23, height
    log = run("charge", "edit:0", 260, "0\n80 cross\n180\n")
    assert "motion 1006" in log and "motion 1007" in log
    # Opening the town menu while guarding must restore normal input after exit.
    log = run("menu-return", "edit:0", 420, """0
80 r1
100 triangle
101
230 circle
231
350 cross
351
""")
    assert "town combat: motion 1005" in log
    assert "town combat: motion 1000" in log
    # Dungeon code uses another CMainChara; the shared Draw override must be inert.
    log = run("dungeon-regression", "dungeon:0", 120, "0\n")
    assert "town combat:" not in log
    # Verify the real audio mixer produces different samples for a stationary
    # attack versus a stationary baseline, not merely a successful SndSePlay call.
    for name, script in [("audio-idle", "0\n"), ("audio-attack", "0\n80 cross\n81\n")]:
        run(name, "edit:0", 200, script, {"DC_AUDIO_WAV": str(OUT / f"{name}.wav")})
    idle = (OUT / "audio-idle.wav").read_bytes()[44:]
    attack = (OUT / "audio-attack.wav").read_bytes()[44:]
    assert len(idle) > 100000 and len(idle) == len(attack)
    assert idle != attack, "attack generated no audible output"
    live_reload()
    print(f"PASS: five-hit combo, repeat attack while moving, jump height, audio, live reload, charge, menu and dungeon. Artifacts: {OUT}")


def live_reload():
    save = OUT / "live-save"
    save.mkdir(exist_ok=True)
    config = save / "town-combat.json"
    config.write_text(json.dumps({"attack_speed": 1.0, "charge_jump_velocity": 2.0}), encoding="utf-8")
    log_path = OUT / "live-reload.log"
    with log_path.open("wb") as output:
        process = subprocess.Popen(
            [str(EXE), "--data", str(ROOT / "port/run/data"), "--save", str(save),
             "--offscreen", "--jump", "edit:0", "--fast-load", "--frames", "5000",
             "--width", "640", "--height", "360"], cwd=ROOT / "port/run",
            stdout=output, stderr=output,
        )
        def wait_for(text):
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                if text in log_path.read_text(encoding="utf-8", errors="replace"):
                    return
                assert process.poll() is None, "game exited before live reload completed"
                time.sleep(0.02)
            raise AssertionError(f"timed out waiting for {text}")
        try:
            wait_for("settings reloaded speed=1.00")
            config.write_text(json.dumps({"attack_speed": 1.5, "charge_jump_velocity": 4.0,
                                          "combo_hits": 3, "charge_frames": 20}), encoding="utf-8")
            wait_for("settings reloaded speed=1.50 jump=4.00 combo=3 charge=20")
            config.write_text('{"charge_jump_velocity": 999}', encoding="utf-8")
            wait_for("keeping previous settings")
            config.write_text(json.dumps({"attack_speed": 1.0, "charge_jump_velocity": 2.0,
                                          "combo_hits": 4}), encoding="utf-8")
            wait_for("settings reloaded speed=1.00 jump=2.00 combo=4")
            assert process.wait(timeout=30) == 0
        finally:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)


if __name__ == "__main__":
    main()
