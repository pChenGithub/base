#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""只读的 Git 仓库巡检脚本。

仅执行 git 的只读子命令（status / diff / log / remote / tag / rev-parse），
不会暂存、提交、推送或修改任何仓库状态，可在任何阶段安全重复执行。

用法:
    python git_report.py [--log-count 10] [--diff-limit 40]

输出内容:
    1. 当前分支与上游跟踪状态（领先 / 落后提交数）
    2. 已暂存 / 未暂存 / 未跟踪文件清单
    3. 改动统计（staged 与 unstaged 分别统计）
    4. 未跟踪文件中的疑似敏感文件告警
    5. 最近提交与最近 tag
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# 疑似敏感文件（未跟踪时需告警，避免误提交）
SENSITIVE_PATTERNS = [
    re.compile(r"(^|/)\.env"),
    re.compile(r"\.(pem|key|p12|pfx|jks|keystore)$", re.IGNORECASE),
    re.compile(r"(^|/)id_(rsa|dsa|ecdsa|ed25519)"),
    re.compile(r"(secret|password|credential|token)", re.IGNORECASE),
]


def run_git(*args: str, check: bool = False) -> tuple[int, str]:
    """执行只读 git 命令并返回 (退出码, 标准输出+标准错误)。"""
    try:
        proc = subprocess.run(
            ["git", "--no-pager", *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
        )
    except FileNotFoundError:
        print("[错误] 未找到 git 可执行文件，请确认已安装并加入 PATH。")
        sys.exit(1)

    output = (proc.stdout or "") + (proc.stderr or "")
    if check and proc.returncode != 0:
        print(f"$ git {' '.join(args)}\n{output.strip()}\n")
    return proc.returncode, output.strip()


def section(title: str) -> None:
    print(f"\n{'=' * 60}\n{title}\n{'=' * 60}")


def parse_status(branch_line: str) -> tuple[str, dict[str, list[str]]]:
    """解析 `git status --porcelain=v1 --branch` 的输出。"""
    staged: list[str] = []
    unstaged: list[str] = []
    untracked: list[str] = []

    for line in branch_line.splitlines():
        if line.startswith("##"):
            continue
        if len(line) < 3:
            continue
        x, y, path = line[0], line[1], line[3:].strip()
        path = path.split(" -> ")[-1] if " -> " in path else path
        if x == "?" and y == "?":
            untracked.append(path)
            continue
        if x not in (" ", "?"):
            staged.append(f"{x} {path}")
        if y not in (" ", "?"):
            unstaged.append(f"{y} {path}")

    return branch_line, {"staged": staged, "unstaged": unstaged, "untracked": untracked}


def print_sensitive(untracked: list[str]) -> None:
    hits = [p for p in untracked if any(rx.search(p) for rx in SENSITIVE_PATTERNS)]
    if not hits:
        return
    print("\n[风险提示] 未跟踪文件中疑似包含敏感文件，请勿提交：")
    for path in hits:
        print(f"  - {path}")


def main() -> int:
    parser = argparse.ArgumentParser(description="只读 Git 仓库巡检")
    parser.add_argument("--log-count", type=int, default=10, help="展示最近提交条数，默认 10")
    parser.add_argument("--diff-limit", type=int, default=40, help="改动统计最多展示行数，默认 40")
    args = parser.parse_args()

    # 1. 分支与上游
    section("1. 当前分支与上游跟踪")
    code, branch = run_git("rev-parse", "--abbrev-ref", "HEAD")
    if code != 0:
        print("当前目录不是 Git 仓库，或无法解析 HEAD（可能处于空仓库状态）：")
        print(branch)
        print("\n[提示] 若这是 SVN 工作副本，请改用 scripts/svn_report.py。")
        return 1
    print(f"当前分支: {branch}")
    if branch == "HEAD":
        print("[风险提示] 处于 detached HEAD 状态，提交将不会挂到任何分支，请先与用户确认处理方式。")

    code, upstream = run_git("rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{upstream}")
    if code == 0 and upstream:
        print(f"上游分支: {upstream}")
        code, counts = run_git("rev-list", "--left-right", "--count", "@{upstream}...HEAD")
        if code == 0 and counts:
            behind, ahead = (counts.split() + ["0", "0"])[:2]
            print(f"落后上游 {behind} 个提交，领先上游 {ahead} 个提交")
        else:
            print("无法计算领先 / 落后提交数（历史可能不相关）。")
    else:
        print("上游分支: 未配置（首次推送需 git push -u origin <branch>）")

    # 2. 文件清单
    section("2. 文件清单")
    _, porcelain = run_git("status", "--porcelain=v1", "--branch")
    _, groups = parse_status(porcelain)
    for key, label in (("staged", "已暂存"), ("unstaged", "未暂存"), ("untracked", "未跟踪")):
        items = groups[key]
        print(f"\n{label}（{len(items)} 个）")
        if not items:
            print("  （无）")
        for item in items:
            print(f"  {item}")
    print_sensitive(groups["untracked"])

    # 3. 改动统计
    section("3. 改动统计")
    for label, git_args in (("已暂存改动", ("diff", "--cached", "--stat")), ("未暂存改动", ("diff", "--stat"))):
        print(f"\n{label}:")
        _, stat = run_git(*git_args)
        if not stat:
            print("  （无）")
            continue
        lines = stat.splitlines()
        for line in lines[: args.diff_limit]:
            print(f"  {line}")
        if len(lines) > args.diff_limit:
            print(f"  ...（已截断，共 {len(lines)} 行）")

    # 4. 最近提交
    section("4. 最近提交")
    _, logs = run_git("log", f"-{args.log_count}", "--oneline", "--no-decorate")
    print(logs if logs else "  （无提交记录）")

    # 5. 远程与 tag
    section("5. 远程仓库与最近 tag")
    _, remotes = run_git("remote", "-v")
    print(remotes if remotes else "  （未配置远程仓库）")
    _, tags = run_git("tag", "--sort=-v:refname", "-l")
    tag_list = tags.splitlines()[:5] if tags else []
    print("\n最近 tag:")
    if not tag_list:
        print("  （无 tag）")
    for tag in tag_list:
        print(f"  {tag}")

    # 6. 项目版本号（以仓库根目录为准，避免因当前工作目录不同而读不到）
    section("6. 项目版本号")
    code, top_level = run_git("rev-parse", "--show-toplevel")
    repo_root = Path(top_level) if code == 0 and top_level else Path.cwd()
    pkg = repo_root / "package.json"
    print(f"仓库根目录: {repo_root}")
    if pkg.is_file():
        text = pkg.read_text(encoding="utf-8", errors="replace")
        match = re.search(r'"version"\s*:\s*"([^"]+)"', text)
        print(f"package.json version: {match.group(1) if match else '未找到 version 字段'}")
    else:
        print("当前目录下未找到 package.json")

    print("\n[提示] 本脚本为只读巡检，未修改任何仓库状态。")
    print("[提示] 后续任何 add / commit / push / tag 操作都必须先获得用户明确确认。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
