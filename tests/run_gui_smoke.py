"""Run native Qt pages using only loopback endpoints and an isolated config/database."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--app", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
args.output = args.output.resolve()
with tempfile.TemporaryDirectory(prefix="tenco-gui-") as directory:
    root = Path(directory)
    config = json.loads((Path(__file__).resolve().parents[1] / "config.json").read_text(encoding="utf-8"))
    config["network"].update(websocketUrl="ws://127.0.0.1:1", statusReadUrl="http://127.0.0.1:1/table/reads",
        authToken="", chassisAutoReconnect=False)
    config["video"].update(autoStart=False, controlBaseUrl="http://127.0.0.1:1", streamUrl="http://127.0.0.1:1/stream", streamOptions=[])
    config["gimbal"]["enabled"] = False
    config["poseSource"]["enabled"] = config["tracking"]["enabled"] = False
    config["database"].update(sqliteFilePath=str(root / "test.sqlite"), connectionName="smoke")
    file = root / "config.json"
    file.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    result = subprocess.run([str(args.app.resolve()), "--config", str(file), "--smoke-output", str(args.output)],
        cwd=root, creationflags=flags, timeout=30, capture_output=True)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    for name in ("home", "map-route", "map-row", "map-mission", "map-small", "maintenance", "help", "about"):
        assert (args.output / (name + ".png")).stat().st_size > 1000
print("PASS native Qt pages, task tabs, resize, screenshots and teardown")
