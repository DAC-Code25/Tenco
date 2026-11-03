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
    config["video"].update(backend="mjpeg_http", autoStart=False, controlBaseUrl="http://127.0.0.1:1", streamUrl="http://127.0.0.1:1/stream")
    config["gimbal"]["enabled"] = False
    config["poseSource"]["enabled"] = config["tracking"]["enabled"] = False
    config["poseSource"]["baseUrl"] = config["tracking"]["baseUrl"] = "http://127.0.0.1:1/api/v1"
    config["database"].update(sqliteFilePath=str(root / "test.sqlite"), connectionName="smoke")
    file = root / "config.json"
    file.write_text(json.dumps(config, ensure_ascii=False), encoding="utf-8")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    result = subprocess.run([str(args.app.resolve()), "--config", str(file), "--smoke-output", str(args.output)],
        cwd=root, creationflags=flags, timeout=100, capture_output=True)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    for name in ("home", "home-depth", "home-preview-menu", "home-small", "home-hover-tooltip",
                 "home-overview-live", "home-overview-live-small", "home-overview-delayed", "home-overview-paused",
                 "home-overview-recovering", "home-overview-recovering-delayed", "home-overview-turning", "home-overview-waiting", "map-route", "map-route-small", "map-row", "map-row-bottom", "map-small", "maintenance", "maintenance-scroll-safe", "maintenance-ipc-parameters-small",
                 "maintenance-navigation", "help", "about"):
        assert (args.output / (name + ".png")).stat().st_size > 1000
    coverage = json.loads((args.output / "coverage.json").read_text(encoding="utf-8"))
    assert coverage["maintenanceTabCount"] == 5
    assert len(coverage["maintenanceSubPages"]) == 7
    assert coverage["maintenanceLeaveChecks"] == {
        "navigationCancelRetainsEdits": True,
        "escapeRetainsEdits": True,
        "closeCancelRetainsEdits": True,
        "discardRestoresFormWithoutSaving": True,
        "fileBytesUnchanged": True,
    }
    for name in ("maintenance-leave-cancel", "maintenance-leave-discard"):
        assert (args.output / (name + ".png")).stat().st_size > 1000
    assert coverage["taskPages"] == ["常规任务", "直线任务"]
    assert coverage["taskExecutionPanelCount"] == 1
    assert coverage["taskWorkflowSimplified"] is True
    assert [layout["size"] for layout in coverage["cameraControlLayouts"]] == ["1280x900", "1200x780", "1280x900-restored"]
    for layout in coverage["cameraControlLayouts"]:
        controls = layout["controls"]
        assert [control["name"] for control in controls] == ["recordButton", "captureButton", "videoPreviewButton", "cameraPreviewKind"]
        assert all(control["height"] == 64 for control in controls)
        widths = [control["width"] for control in controls]
        assert max(widths) - min(widths) <= 2
    assert coverage["mapEditorTabCount"] == 3
    assert [page["title"] for page in coverage["mapEditors"]] == ["点位", "路径", "地图"]
    assert [page["index"] for page in coverage["mapEditors"]] == list(range(3))
    for page in coverage["mapEditors"]:
        for position in ("top", "bottom"):
            assert (args.output / (page[position] + ".png")).stat().st_size > 1000
    assert "map-mission" not in coverage["screenshots"]
    assert coverage["ipcMaintenanceTabCount"] == 5
    ipc_pages = coverage["ipcMaintenance"]
    assert [page["index"] for page in ipc_pages] == list(range(5))
    assert [page["title"] for page in ipc_pages] == ["服务管理", "融合与导航参数", "配置备份", "相机采集", "车辆与原点 · 只读"]
    for page in ipc_pages:
        for position in ("top", "bottom"):
            assert (args.output / (page[position] + ".png")).stat().st_size > 1000
    for page in coverage["maintenanceSubPages"]:
        assert page["title"]
        for position in ("top", "bottom"):
            assert (args.output / (page[position] + ".png")).stat().st_size > 1000
    assert coverage["helpTopicCount"] >= 14
    for key, count_key in (("maintenance", "maintenanceTabCount"), ("help", "helpTopicCount")):
        pages = coverage[key]
        assert len(pages) == coverage[count_key]
        assert [page["index"] for page in pages] == list(range(coverage[count_key]))
        assert all(page["title"] for page in pages)
        for page in pages:
            for position in ("top", "bottom"):
                assert (args.output / (page[position] + ".png")).stat().st_size > 1000
    assert coverage["aboutScrollPositions"] == ["top", "middle", "bottom"]
    assert len(coverage["screenshots"]) == len(set(coverage["screenshots"]))
    for name in coverage["screenshots"]:
        assert (args.output / (name + ".png")).stat().st_size > 1000
    for name in ("help-search-match", "help-search-empty", "about-top", "about-middle", "about-bottom"):
        assert (args.output / (name + ".png")).stat().st_size > 1000
print("PASS read-only home overview, stale status, tooltips, two task pages, three map editors, fixed execution panel, RGB/depth preview selection, maintenance, wheel-safe scrolling and teardown")
