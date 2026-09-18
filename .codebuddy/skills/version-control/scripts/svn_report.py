#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""只读的 SVN 工作副本巡检脚本。

仅执行 svn 的只读子命令（info / status / diff / log / list / propget），
不会 add、commit、revert、update 或修改任何工作副本与版本库状态，可安全重复执行。

用法:
    python svn_report.py [--log-count 10] [--diff-limit 40]

输出内容:
    1. 工作副本信息（URL / 工作副本根 / 仓库根 / 当前 revision）
    2. 本地状态清单（含属性修改、锁定、冲突、外部引用）
    3. 服务端新版本检测（svn status -u 原文 + out-of-date 计数）
    4. 改动统计（按 Index 统计文件数与增删行数）
    5. 未版本化文件中的疑似敏感文件告警
    6. 最近提交与 tags 目录
    7. svn:ignore 与项目版本号

说明:
    - 所有命令附加 --non-interactive，避免因等待输入凭据而挂起；
      若因缺少凭据失败，会打印原始错误并继续输出其余信息。
    - 解析为尽力而为（best-effort），遇到无法识别的输出会原样打印，便于人工判读。
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# 疑似敏感文件（未版本化时需告警，禁止 svn add）
SENSITIVE_PATTERNS = [
    re.compile(r"(^|/)\.env"),
    re.compile(r"\.(pem|key|p12|pfx|jks|keystore)$", re.IGNORECASE),
    re.compile(r"(^|/)id_(rsa|dsa|ecdsa|ed25519)"),
    re.compile(r"(secret|password|credential|token)", re.IGNORECASE),
]

# 本地状态字符含义
STATUS_MEANING = {
    "A": "新增（已计划纳入版本控制）",
    "M": "内容已修改",
    "D": "删除（已计划删除）",
    "R": "替换（移动或复制产生）",
    "C": "冲突",
    "?": "未版本化（不会随提交上传）",
    "!": "缺失（版本控制中但本地文件不存在）",
    "I": "已被忽略",
    "X": "外部引用（svn:externals）",
    "~": "被其他类型资源占位",
    "L": "已加锁",
    "S": "已切换（switch）",
}

TIMEOUT = 30


def run_svn(*args: str) -> tuple[int, str]:
    """执行只读 svn 命令并返回 (退出码, 输出)。附加 --non-interactive 防止挂起。"""
    cmd = ["svn", *args, "--non-interactive"]
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=False,
            timeout=TIMEOUT,
        )
    except FileNotFoundError:
        print("[错误] 未找到 svn 可执行文件。请先安装 SVN 命令行客户端（如 TortoiseSVN 需勾选 command line client tools）。")
        sys.exit(1)
    except subprocess.TimeoutExpired:
        return 1, f"[超时] svn {' '.join(args)} 超过 {TIMEOUT}s 未返回（可能网络或凭据问题）"

    output = (proc.stdout or "") + (proc.stderr or "")
    return proc.returncode, output.strip()


def section(title: str) -> None:
    print(f"\n{'=' * 60}\n{title}\n{'=' * 60}")


def parse_info(output: str) -> dict[str, str]:
    """解析 `svn info` 的键值输出。"""
    info: dict[str, str] = {}
    for line in output.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        info[key.strip()] = value.strip()
    return info


def parse_status(output: str) -> list[tuple[str, str]]:
    """解析 `svn status` 输出为 (状态列, 路径) 列表。状态区按 SVN 约定取前 8 列。"""
    entries: list[tuple[str, str]] = []
    for line in output.splitlines():
        if not line.strip() or line.startswith("---") or line.startswith("Status against revision"):
            continue
        status_cols = line[:8].rstrip()
        path = line[8:].strip()
        if not path:  # 兜底：状态与路径之间空格数异常时按空白切分
            parts = line.split(None, 1)
            status_cols = parts[0] if parts else ""
            path = parts[1] if len(parts) > 1 else ""
        if not status_cols and not path:
            continue
        entries.append((status_cols, path))
    return entries


def count_diff(output: str) -> tuple[int, int, int]:
    """统计 `svn diff` 输出中的文件数、新增行数、删除行数。"""
    files = len(re.findall(r"^Index: ", output, flags=re.MULTILINE))
    added = len([ln for ln in output.splitlines() if ln.startswith("+") and not ln.startswith("+++")])
    removed = len([ln for ln in output.splitlines() if ln.startswith("-") and not ln.startswith("---")])
    return files, added, removed


def find_upward(name: str) -> Path | None:
    """自当前目录向上查找文件。"""
    current = Path.cwd()
    for parent in [current, *current.parents]:
        candidate = parent / name
        if candidate.is_file():
            return candidate
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="只读 SVN 工作副本巡检")
    parser.add_argument("--log-count", type=int, default=10, help="展示最近提交条数，默认 10")
    parser.add_argument("--diff-limit", type=int, default=40, help="改动统计最多展示行数，默认 40")
    args = parser.parse_args()

    # 1. 工作副本信息
    section("1. 工作副本信息")
    code, info_out = run_svn("info")
    if code != 0:
        print("当前目录不是 SVN 工作副本（svn info 失败）：")
        print(info_out)
        print("\n[提示] 若这是 Git 仓库，请改用 scripts/git_report.py。")
        return 1
    info = parse_info(info_out)
    for key in ("Path", "Working Copy Root Path", "URL", "Relative URL", "Repository Root", "Repository UUID", "Revision", "Node Kind", "Schedule"):
        if key in info:
            print(f"{key}: {info[key]}")
    revision = info.get("Revision", "?")
    last_changed = info.get("Last Changed Rev", "")
    if last_changed and last_changed != revision:
        print(f"[提示] 本地 revision({revision}) 与 Last Changed Rev({last_changed}) 不一致，属 mixed-revision 常态，提交前会统一到最新。")

    # 2. 本地状态
    section("2. 本地状态清单")
    _, status_out = run_svn("status")
    entries = parse_status(status_out)
    if not entries:
        print("（无改动，工作副本干净）")
    else:
        print(f"共 {len(entries)} 项：\n")
        print(f"{'状态':<10}{'含义':<28}路径")
        for cols, path in entries:
            meaning_parts = [STATUS_MEANING.get(ch, f"未知标记 {ch}") for ch in cols if ch.strip()]
            print(f"{cols or '-':<10}{' / '.join(meaning_parts) or '-':<28}{path}")
        conflicts = [p for cols, p in entries if "C" in cols]
        if conflicts:
            print("\n[风险提示] 存在冲突项，必须先解决冲突再考虑提交：")
            for path in conflicts:
                print(f"  - {path}")
        externals = [p for cols, p in entries if "X" in cols]
        if externals:
            print("\n[提示] 存在 svn:externals 外部引用，其内容不属于本工作副本的提交范围：")
            for path in externals:
                print(f"  - {path}")

    # 3. 服务端新版本检测
    section("3. 服务端新版本检测（svn status -u）")
    code, status_u = run_svn("status", "-u")
    if code != 0:
        print("无法检查服务端状态（可能需要先配置凭据或网络不可达）：")
        print(status_u)
    else:
        out_of_date = [ln for ln in status_u.splitlines() if "*" in ln[:12] and not ln.lstrip().startswith("*.")]
        against = [ln for ln in status_u.splitlines() if ln.startswith("Status against revision")]
        if against:
            print(against[0])
        print(f"服务端存在新版本（out-of-date，带 * 标记）的条目数：{len(out_of_date)}")
        if out_of_date:
            print("\n[风险提示] 以下条目服务端已有更新，提交前必须先与用户确认是否 svn update：")
            for line in out_of_date:
                print(f"  {line}")
        print("\n原始输出（供人工判读，必要时对照上方计数）：")
        for line in (status_u.splitlines() or ["（无输出）"]):
            print(f"  {line}")

    # 4. 改动统计
    section("4. 改动统计")
    _, diff_out = run_svn("diff")
    files, added, removed = count_diff(diff_out)
    print(f"改动文件数: {files}，新增行: {added}，删除行: {removed}")
    if not diff_out:
        print("（无未提交改动）")
    else:
        hunk_headers = re.findall(r"^Index: (.+)$", diff_out, flags=re.MULTILINE)
        for path in hunk_headers[: args.diff_limit]:
            print(f"  - {path}")
        if len(hunk_headers) > args.diff_limit:
            print(f"  ...（已截断，共 {len(hunk_headers)} 个文件）")

    # 5. 敏感文件告警
    section("5. 未版本化敏感文件告警")
    unversioned = [p for cols, p in entries if "?" in cols]
    hits = [p for p in unversioned if any(rx.search(p) for rx in SENSITIVE_PATTERNS)]
    if hits:
        print("[风险提示] 以下未版本化文件疑似敏感文件，禁止 svn add：")
        for path in hits:
            print(f"  - {path}")
    else:
        print("（未发现疑似敏感的未版本化文件）")
    print(f"未版本化文件总数: {len(unversioned)}")

    # 6. 最近提交与 tags
    section("6. 最近提交与 tags")
    code, log_out = run_svn("log", f"-l{args.log_count}")
    print("最近提交:")
    if code != 0:
        print(log_out)
    else:
        for line in log_out.splitlines():
            if line.startswith("---") or not line.strip():
                continue
            print(f"  {line}")

    repo_root = info.get("Repository Root", "")
    print("\ntags:")
    if not repo_root:
        print("（无法确定仓库根 URL，跳过 tags 列表）")
    else:
        code, tag_out = run_svn("list", f"{repo_root.rstrip('/')}/tags")
        if code != 0:
            print(f"（读取 {repo_root}/tags 失败，可能不符合 trunk/branches/tags 布局）")
            print(tag_out)
        else:
            tag_list = [ln.strip() for ln in tag_out.splitlines() if ln.strip()]
            for tag in tag_list[-5:]:
                print(f"  {tag}")
            if not tag_list:
                print("  （tags 目录为空）")

    # 7. 忽略规则与项目版本号
    section("7. svn:ignore 与项目版本号")
    _, ignore_out = run_svn("propget", "svn:ignore", ".")
    print("当前目录 svn:ignore:")
    print(f"  {ignore_out if ignore_out else '（未设置）'}")

    pkg = find_upward("package.json")
    if pkg:
        text = pkg.read_text(encoding="utf-8", errors="replace")
        match = re.search(r'"version"\s*:\s*"([^"]+)"', text)
        print(f"\npackage.json: {pkg}")
        print(f"version: {match.group(1) if match else '未找到 version 字段'}")
    else:
        print("\n未找到 package.json（已自当前目录向上查找）")

    print("\n[提示] 本脚本为只读巡检，未修改任何工作副本或版本库状态。")
    print("[提示] SVN 提交直接落服务器且无 amend，任何 add / commit / revert / update / copy(tag) 都必须先获得用户明确确认。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
