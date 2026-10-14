#!/usr/bin/env python3
"""Maintain canonical workspace documentation and independent repository snapshots.

Run snapshot, sync, then check from the complete workspace. Independently cloned
repository snapshots support check --documents-only; this is not a code audit.
Only the Python standard library and Git are required.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import quote, unquote, urlsplit


ROOT = Path(__file__).resolve().parents[1]
SESSION_RE = re.compile(r"^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$")
LINK_RE = re.compile(r"(?<!!)\[([^\]\n]+)\]\(([^\n)]+)\)")
PRIMARY = (
    "AGENTS.md", "docs/README.md", "docs/项目总览.md", "docs/当前状态.md",
    "docs/变更记录.md", "docs/问题与待办.md", "docs/会话索引.md",
)
LIVE = ("docs/当前状态.md", "docs/变更记录.md", "docs/问题与待办.md", "docs/会话索引.md")
CHECKED = PRIMARY + ("docs/develop/项目文档治理开发方案.md", "docs/develop/项目文档治理开发记录.md")
FILES = PRIMARY + (
    "docs/项目状态.json", "docs/templates/开发方案模板.md",
    "docs/templates/开发记录模板.md", "docs/templates/会话交接模板.md",
    "docs/develop/项目文档治理开发方案.md", "docs/develop/项目文档治理开发记录.md",
    "tools/project_docs.py", "工控机控制闭环与位姿直连系统开发方案.md",
    "docs/系统架构与测试覆盖审查-2026-09-19.md",
    "docs/位姿地图全链路修复与验证-2026-09-19.md",
    "tools/tests/test_project_docs.py",
)
REPOSITORIES = {
    "src": ("origin", "main", "DAC-Code25/sensor-fusion-ros2"),
    "Tenco": ("Tenco", "master", "DAC-Code25/Tenco"),
    "oak-camera-service": ("origin", "main", "DAC-Code25/oak-camera-service"),
}
ENTRY = """# 项目统一规范入口

本仓库与其他独立子项目共同组成工控机/Tenco/相机服务工作空间。

1. 完整工作空间中，读取 [根 AGENTS](../AGENTS.md)，再按其顺序读取根 `docs/` 的当前状态、问题、变更、会话和总览。
2. 独立克隆时，读取 [统一规范快照](docs/workspace/AGENTS.md) 与 [文档入口](docs/workspace/docs/README.md)；该快照注明日期、会话及各仓功能基线，不证明其他仓库或实车当前状态。
3. 本文件只提供入口，不另定义控制、Git、构建或记录规则。所有约束编辑源是工作空间根 AGENTS。
4. `docs/workspace/` 由根 `tools/project_docs.py sync` 生成，禁止独立手改；独立克隆检查使用 `python docs/workspace/tools/project_docs.py check --documents-only`。

入口生成标记：workspace-docs-entry-v1。
"""


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def document_bytes(path: Path) -> bytes:
    # Git/Windows may check text files out as CRLF. Snapshot integrity ignores
    # that representation change while preserving every other byte.
    return path.read_bytes().replace(b"\r\n", b"\n")


def manifest_digest(path: Path, manifest: dict) -> str:
    # The initial migration manifest used raw bytes; preserve its integrity
    # check until sync replaces it with the normalized text manifest.
    return sha(document_bytes(path) if manifest.get("hash_normalization") else path.read_bytes())


def git(repo: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-c", "core.quotepath=false", "-C", str(repo), *args], stderr=subprocess.PIPE
    ).decode("utf-8").strip()


def is_functional(path: str) -> bool:
    # Documentation may be committed after the recorded functional baseline.
    return not (path.startswith("docs/") or path.lower().endswith(".md"))


def fingerprint(repo: Path) -> tuple[str, int]:
    raw = subprocess.check_output(
        ["git", "-C", str(repo), "ls-files", "-z", "--cached", "--others", "--exclude-standard"]
    )
    paths = sorted({p.decode("utf-8") for p in raw.split(b"\0") if p})
    digest = hashlib.sha256()
    count = 0
    for relative in paths:
        if not is_functional(relative):
            continue
        p = repo / relative
        if not p.exists() and not p.is_symlink():
            content = b"MISSING"
        elif p.is_symlink():
            content = b"SYMLINK\0" + p.readlink().as_posix().encode("utf-8")
        else:
            content = p.read_bytes()
        digest.update(relative.encode("utf-8") + b"\0" + sha(content).encode("ascii") + b"\0")
        count += 1
    return digest.hexdigest(), count


def write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or document_bytes(path) != data:
        path.write_bytes(data)


def load_state(root: Path) -> dict:
    return json.loads((root / "docs/项目状态.json").read_text(encoding="utf-8"))


def check_fields(root: Path, session: str, date: str) -> list[str]:
    errors = []
    for name in PRIMARY:
        path = root / name
        if not path.is_file():
            errors.append(f"缺少必需文档：{name}")
            continue
        text = path.read_text(encoding="utf-8")
        date_match = re.search(r"^更新日期：([0-9]{4}-[0-9]{2}-[0-9]{2})$", text, re.M)
        session_match = re.search(r"^会话 ID：`([^`]+)`$", text, re.M)
        if not date_match:
            errors.append(f"{name}：缺少更新日期")
        else:
            try:
                actual_date = dt.date.fromisoformat(date_match.group(1))
                if actual_date > dt.date.fromisoformat(date):
                    errors.append(f"{name}：更新日期晚于快照")
                if name in LIVE and date_match.group(1) != date:
                    errors.append(f"{name}：滚动记录更新日期与快照不一致")
            except ValueError:
                errors.append(f"{name}：更新日期非法")
        if not session_match or not SESSION_RE.fullmatch(session_match.group(1)):
            errors.append(f"{name}：缺少或非法会话 ID")
        elif name in LIVE and session_match.group(1) != session:
            errors.append(f"{name}：滚动记录未关联当前会话 ID")
    return errors


def local_target(root: Path, document: str, target: str) -> tuple[Path, str] | None:
    target = target.strip().strip("<>")
    if target.startswith("#"):
        return None
    windows = re.match(r"^/?[A-Za-z]:[\\/]", target)
    if not windows and urlsplit(target).scheme:
        return None
    path, _, anchor = target.partition("#")
    line = re.search(r":([0-9]+)$", path)
    if line:
        anchor = "L" + line.group(1)
        path = path[:line.start()]
    path = unquote(path)
    if windows:
        path = path.lstrip("/")
        resolved = Path(path).resolve()
    else:
        resolved = ((root / document).parent / path).resolve()
    return resolved, anchor


def check_links(root: Path, allow_snapshot_output: bool = False) -> list[str]:
    errors = []
    for name in CHECKED:
        path = root / name
        if not path.is_file():
            continue
        for _, target in LINK_RE.findall(path.read_text(encoding="utf-8")):
            result = local_target(root, name, target)
            if result and not result[0].exists():
                if allow_snapshot_output and result[0] == (root / "docs/项目状态.json").resolve():
                    continue
                errors.append(f"{name}：链接不存在：{target}")
    return errors


def mirror_content(root: Path, name: str, state: dict) -> bytes:
    data = document_bytes(root / name)
    if not name.endswith(".md"):
        return data
    known = {(root / n).resolve() for n in FILES}

    def replace(match: re.Match) -> str:
        label, target = match.groups()
        result = local_target(root, name, target)
        if not result:
            return match.group(0)
        path, anchor = result
        if path in known:
            return match.group(0)
        for repo_name, (_, _, slug) in REPOSITORIES.items():
            try:
                relative = path.relative_to((root / repo_name).resolve())
            except ValueError:
                continue
            # Task branches are deleted after integration. Current documents
            # must retain working cross-repository links after that cleanup.
            branch = state["repositories"][repo_name]["default_branch"]
            url = f"https://github.com/{slug}/blob/{quote(branch, safe='/')}/{quote(relative.as_posix(), safe='/')}"
            return f"[{label}]({url}{'#' + anchor if anchor else ''})"
        # Untracked local evidence remains named, without a broken mirror link.
        try:
            local = path.relative_to(root.resolve()).as_posix()
        except ValueError:
            local = str(path)
        return f"{label}（本地工作空间：`{local}`）"

    return LINK_RE.sub(replace, data.decode("utf-8")).encode("utf-8")


def snapshot(root: Path, session: str) -> None:
    if not SESSION_RE.fullmatch(session):
        raise ValueError("session-id 必须为当前会话的完整 UUID")
    now = dt.datetime.now(dt.timezone(dt.timedelta(hours=8))).replace(microsecond=0)
    errors = check_fields(root, session, now.date().isoformat()) + check_links(root, allow_snapshot_output=True)
    if errors:
        raise ValueError("先完成人工记录更新：\n" + "\n".join(errors))
    previous_path = root / "docs/项目状态.json"
    previous = load_state(root) if previous_path.exists() else {"repositories": {}}
    repositories = {}
    for name, (remote, default, _) in REPOSITORIES.items():
        repo = root / name
        digest, count = fingerprint(repo)
        branch = git(repo, "branch", "--show-current")
        if not branch:
            raise ValueError(f"{name} 处于 detached HEAD，须先明确工作分支")
        head = git(repo, "rev-parse", "HEAD")
        baseline = head
        old = previous.get("repositories", {}).get(name, {})
        if old.get("functional_fingerprint") == digest and old.get("code_baseline"):
            ancestor = subprocess.run(["git", "-C", str(repo), "merge-base", "--is-ancestor", old["code_baseline"], head], capture_output=True)
            if ancestor.returncode == 0:
                baseline = old["code_baseline"]
        tracked_dirty = [p for p in git(repo, "diff", "HEAD", "--name-only").splitlines() if is_functional(p)]
        untracked = [p for p in git(repo, "ls-files", "--others", "--exclude-standard").splitlines() if is_functional(p)]
        repositories[name] = {
            "branch": branch, "code_baseline": baseline, "checkpoint_head": head, "remote": remote,
            "default_branch": default, "default_baseline": git(repo, "rev-parse", f"{remote}/{default}"),
            "functional_fingerprint": digest, "functional_file_count": count,
            "functional_tracked_changes": tracked_dirty,
            "functional_untracked_files": untracked,
        }
    state = {
        "schema_version": 1, "updated_at": now.isoformat(), "session_id": session,
        "timezone": "Asia/Shanghai", "repositories": repositories,
        "scope": "本机源码与文档快照；非实车或另一克隆的实时状态",
        "fingerprint_scope": "Git 纳管/未忽略文件，排除 docs/ 与 Markdown；检测源码、配置、测试和构建变化",
    }
    write(root / "docs/项目状态.json", (json.dumps(state, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(f"已采集 {len(repositories)} 个仓库的分支、功能基线与源码指纹；请 sync 并 check。")


def expected_manifest(root: Path, state: dict) -> tuple[dict, dict[str, bytes]]:
    rendered = {name: mirror_content(root, name, state) for name in FILES}
    manifest = {
        "schema_version": 1, "snapshot_at": state["updated_at"], "session_id": state["session_id"],
        "canonical_source": "完整工作空间根目录，禁止独立编辑本镜像",
        "hash_normalization": "UTF-8 文本的 CRLF 规范化为 LF；其他字节不变",
        "files": {name: {"source_sha256": sha(document_bytes(root / name)), "mirror_sha256": sha(data)} for name, data in rendered.items()},
        "entry_sha256": sha(ENTRY.encode("utf-8")),
    }
    return manifest, rendered


def sync(root: Path, initialize_entries: bool) -> None:
    state = load_state(root)
    errors = check_fields(root, state["session_id"], dt.datetime.fromisoformat(state["updated_at"]).date().isoformat()) + check_links(root)
    if errors:
        raise ValueError("先更新说明并重新 snapshot：\n" + "\n".join(errors))
    manifest, rendered = expected_manifest(root, state)
    # Validate every destination before performing any writes.
    for name in REPOSITORIES:
        repo = root / name
        entry = repo / "AGENTS.md"
        if not (repo / ".git").exists():
            raise ValueError(f"缺少仓库：{repo}")
        if entry.exists() and document_bytes(entry) != ENTRY.encode("utf-8") and not initialize_entries:
            raise ValueError(f"{entry} 含非生成内容；首次迁移需显式 --initialize-entries，先审阅其规则")
        folder = repo / "docs/workspace"
        previous = folder / "镜像清单.json"
        old = json.loads(previous.read_text(encoding="utf-8")) if previous.exists() else {"files": {}}
        existing = {p.relative_to(folder).as_posix(): p for p in folder.rglob("*") if p.is_file()} if folder.exists() else {}
        for relative, path in existing.items():
            if relative == "镜像清单.json":
                continue
            record = old["files"].get(relative)
            if not record or manifest_digest(path, old) != record["mirror_sha256"]:
                raise ValueError(f"镜像存在未知或手改文件，拒绝覆盖：{path}")
            if relative not in rendered:
                raise ValueError(f"清单范围发生删除，请先明确处理旧文件：{path}")
    for name in REPOSITORIES:
        repo = root / name
        write(repo / "AGENTS.md", ENTRY.encode("utf-8"))
        for relative, data in rendered.items():
            write(repo / "docs/workspace" / relative, data)
        write(repo / "docs/workspace/镜像清单.json", (json.dumps(manifest, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    print(f"已同步统一入口和 {len(rendered)} 个文件到 {len(REPOSITORIES)} 个仓库；未复制原始对话或附件。")


def check(root: Path, documents_only: bool) -> list[str]:
    errors = []
    try:
        state = load_state(root)
        session = state["session_id"]
        parsed_date = dt.datetime.fromisoformat(state["updated_at"])
        if state.get("schema_version") != 1 or not SESSION_RE.fullmatch(session):
            errors.append("机器快照版本或会话 ID 非法")
        if parsed_date.tzinfo is None:
            errors.append("机器快照更新时间缺少时区")
        date = parsed_date.date().isoformat()
        errors.extend(check_fields(root, session, date))
        errors.extend(check_links(root))
        issues = (root / "docs/问题与待办.md").read_text(encoding="utf-8")
        issue_ids = re.findall(r"^\| ((?:P|D|R)-[0-9]{3}) \|", issues, re.M)
        if len(issue_ids) != len(set(issue_ids)):
            errors.append("问题 ID 重复")
        known_ids = set(issue_ids)
        for name in PRIMARY:
            for value in set(re.findall(r"\b(?:P|D|R)-[0-9]{3}\b", (root / name).read_text(encoding="utf-8"))):
                if value not in known_ids:
                    errors.append(f"{name}：引用未登记的问题 {value}")
        sessions = (root / "docs/会话索引.md").read_text(encoding="utf-8")
        if f"## {session}" not in sessions:
            errors.append("当前会话未登记到会话索引")
        for name in FILES:
            if not (root / name).is_file():
                errors.append(f"镜像源文件缺失：{name}")
        if documents_only:
            manifest = json.loads((root / "镜像清单.json").read_text(encoding="utf-8"))
            if manifest["session_id"] != session or manifest["snapshot_at"] != state["updated_at"]:
                errors.append("独立快照清单元数据不一致")
            if set(manifest["files"]) != set(FILES):
                errors.append("独立快照清单文件范围不完整")
            for name, record in manifest["files"].items():
                path = root / name
                if not path.is_file() or manifest_digest(path, manifest) != record["mirror_sha256"]:
                    errors.append(f"独立快照摘要不一致：{name}")
            actual = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
            if actual - set(FILES) - {"镜像清单.json"}:
                errors.append("独立快照含未登记文件")
            return errors
        for name in REPOSITORIES:
            repo = root / name
            recorded = state["repositories"][name]
            if git(repo, "branch", "--show-current") != recorded["branch"]:
                errors.append(f"{name}：分支与快照不一致，更新状态/会话后重新 snapshot")
            digest, _ = fingerprint(repo)
            if digest != recorded["functional_fingerprint"]:
                errors.append(f"{name}：功能源码/配置/测试变化尚未更新快照和说明")
            if len(recorded["code_baseline"]) != 40:
                errors.append(f"{name}：功能基线不是完整 SHA")
            else:
                subprocess.run(["git", "-C", str(repo), "cat-file", "-e", recorded["code_baseline"] + "^{commit}"], check=True, capture_output=True)
        if errors:
            return errors
        expected, rendered = expected_manifest(root, state)
        for name in REPOSITORIES:
            repo = root / name
            if not (repo / "AGENTS.md").is_file() or document_bytes(repo / "AGENTS.md") != ENTRY.encode("utf-8"):
                errors.append(f"{name}：AGENTS 入口未同步")
            folder = repo / "docs/workspace"
            stored = folder / "镜像清单.json"
            if not stored.is_file() or json.loads(stored.read_text(encoding="utf-8")) != expected:
                errors.append(f"{name}：镜像清单未同步")
            for relative, data in rendered.items():
                path = folder / relative
                if not path.is_file() or document_bytes(path) != data:
                    errors.append(f"{name}：镜像过时或被手改：{relative}")
            actual = {p.relative_to(folder).as_posix() for p in folder.rglob("*") if p.is_file()}
            extra = actual - set(rendered) - {"镜像清单.json"}
            if extra:
                errors.append(f"{name}：镜像含未登记文件：{sorted(extra)}")
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        errors.append(f"校验无法完成：{exc}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("snapshot", "sync", "check"))
    parser.add_argument("--root", type=Path, default=ROOT, help="完整工作空间根，或独立镜像根")
    parser.add_argument("--session-id", help="snapshot 使用的真实当前会话 UUID")
    parser.add_argument("--documents-only", action="store_true", help="check 独立克隆文档快照，不检查整个项目源码")
    parser.add_argument("--initialize-entries", action="store_true", help="sync 迁移已审阅的旧 AGENTS 入口")
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8")
    try:
        root = args.root.resolve()
        if args.command == "snapshot":
            if not args.session_id:
                parser.error("snapshot 需要 --session-id")
            snapshot(root, args.session_id)
        elif args.command == "sync":
            sync(root, args.initialize_entries)
        else:
            errors = check(root, args.documents_only)
            if errors:
                print("文档检查失败：\n" + "\n".join(errors))
                return 1
            mode = "独立文档快照（未验证整个工作空间源码）" if args.documents_only else f"完整工作空间文档、源码指纹和 {len(REPOSITORIES)} 个仓库镜像"
            print(f"检查通过：{mode}。")
    except (OSError, ValueError, KeyError, TypeError, subprocess.CalledProcessError) as exc:
        print(f"操作失败：{exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
