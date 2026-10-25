"""Profile the copied-save streaming regression, preserving its pass/fail result."""
import os
import subprocess
import sys
from pathlib import Path
from analyze_world_profile import analyze
root=Path(__file__).resolve().parents[1]
path=root/'port/run/mod-tests/integration/world-profile.jsonl'
path.parent.mkdir(parents=True,exist_ok=True)
env={**os.environ,'DC_WORLD_PROFILE':str(path)}
result=subprocess.run([sys.executable,str(root/'tools/test_world_streaming.py')],env=env,cwd=root)
if path.exists():analyze(path)
sys.exit(result.returncode)
