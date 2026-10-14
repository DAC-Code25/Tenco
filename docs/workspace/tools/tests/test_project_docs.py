"""Exercise workspace snapshots against three real, isolated Git repositories."""
from __future__ import annotations

import datetime as dt
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, "reconfigure"):
        stream.reconfigure(encoding="utf-8")

SOURCE = Path(__file__).resolve().parents[1] / "project_docs.py"
SPEC = importlib.util.spec_from_file_location("project_docs", SOURCE)
assert SPEC and SPEC.loader
DOCS = importlib.util.module_from_spec(SPEC)
# The same test is shipped inside immutable documentation mirrors.
# Loading its tool must not leave a generated cache in that mirror.
PREVIOUS_BYTECODE = sys.dont_write_bytecode
sys.dont_write_bytecode = True
try:
    SPEC.loader.exec_module(DOCS)
finally:
    sys.dont_write_bytecode = PREVIOUS_BYTECODE
SESSION = "01a13477-9b6f-7f43-8a41-5e5b77886c15"


class WorkspaceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        date = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).date().isoformat()
        header = f"更新日期：{date}\n会话 ID：`{SESSION}`\n"
        for name in DOCS.FILES:
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(header, encoding="utf-8")
        (self.root / "docs/会话索引.md").write_text(header + f"\n## {SESSION}\n", encoding="utf-8")
        (self.root / "docs/项目状态.json").write_text('{"repositories": {}}\n', encoding="utf-8")
        for name, (remote, default, _) in DOCS.REPOSITORIES.items():
            repo = self.root / name
            repo.mkdir()
            self.run_git(repo, "init", "-b", default)
            self.run_git(repo, "config", "user.name", "文档验证")
            self.run_git(repo, "config", "user.email", "docs-test@example.invalid")
            self.run_git(repo, "config", "core.autocrlf", "false")
            self.run_git(repo, "config", "commit.gpgsign", "false")
            (repo / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
            self.run_git(repo, "add", "main.cpp")
            self.run_git(repo, "commit", "-m", "验证基线")
            self.run_git(repo, "update-ref", f"refs/remotes/{remote}/{default}", "HEAD")
        DOCS.snapshot(self.root, SESSION)
        DOCS.sync(self.root, False)

    @staticmethod
    def run_git(repo, *args):
        subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True)

    def test_three_repositories_and_independent_snapshots(self):
        self.assertEqual(set(DOCS.load_state(self.root)["repositories"]), set(DOCS.REPOSITORIES))
        self.assertEqual(DOCS.check(self.root, False), [])
        for name in DOCS.REPOSITORIES:
            self.assertEqual(DOCS.check(self.root / name / "docs/workspace", True), [])

    def test_camera_source_change_requires_new_snapshot(self):
        path = self.root / "oak-camera-service/main.cpp"
        path.write_text("int main() { return 1; }\n", encoding="utf-8")
        self.assertTrue(any("oak-camera-service" in e for e in DOCS.check(self.root, False)))
        DOCS.snapshot(self.root, SESSION)
        DOCS.sync(self.root, False)
        self.assertEqual(DOCS.check(self.root, False), [])

    def test_camera_branch_change_is_detected(self):
        self.run_git(self.root / "oak-camera-service", "switch", "-c", "chore/test")
        self.assertTrue(any("oak-camera-service" in e and "分支" in e for e in DOCS.check(self.root, False)))

    def test_camera_mirror_tampering_is_rejected_before_any_writes(self):
        path = self.root / "oak-camera-service/docs/workspace/AGENTS.md"
        path.write_text("手改镜像\n", encoding="utf-8")
        other = self.root / "Tenco/docs/workspace/镜像清单.json"
        before = other.read_bytes()
        with self.assertRaisesRegex(ValueError, "拒绝覆盖"):
            DOCS.sync(self.root, False)
        self.assertEqual(other.read_bytes(), before)
        self.assertTrue(DOCS.check(self.root / "oak-camera-service/docs/workspace", True))

    def test_camera_links_render_to_its_own_repository(self):
        self.run_git(self.root / "oak-camera-service", "switch", "-c", "chore/temporary-task")
        path = self.root / "oak-camera-service/README.md"
        path.write_text("相机说明\n", encoding="utf-8")
        document = self.root / "docs/项目总览.md"
        with document.open("a", encoding="utf-8") as stream:
            stream.write("\n[相机](../oak-camera-service/README.md)\n")
        DOCS.snapshot(self.root, SESSION)
        DOCS.sync(self.root, False)
        rendered = (self.root / "Tenco/docs/workspace/docs/项目总览.md").read_text(encoding="utf-8")
        self.assertIn("https://github.com/DAC-Code25/oak-camera-service/blob/main/README.md", rendered)
        self.assertEqual(DOCS.check(self.root, False), [])

    def test_missing_third_repository_is_reported(self):
        state = DOCS.load_state(self.root)
        del state["repositories"]["oak-camera-service"]
        (self.root / "docs/项目状态.json").write_text(json.dumps(state), encoding="utf-8")
        self.assertTrue(DOCS.check(self.root, False))


if __name__ == "__main__":
    unittest.main()
