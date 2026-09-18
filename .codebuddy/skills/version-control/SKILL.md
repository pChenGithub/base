---
name: version-control
description: |
  独立、自包含的用户主导版本控制 skill（Git / SVN 双栈）：查看改动、生成中文+类型前缀的提交信息、提交、推送或更新、版本号与 tag 发布。
  固定步骤机械执行、只做必要思考；任何写操作前先输出提案并停下等待用户明确确认，禁止自作主张。
  触发词：提交代码、提交改动、commit、svn commit、推送、push、svn update、发版、打标签、tag、版本号、CHANGELOG、查看改动、版本管理、git 状态、svn 状态。
---

# 版本控制（用户主导，Git / SVN 双栈）

> **独立声明**：本 skill 自包含，**不依赖、不调用任何其他 skill，也不触发任何外部工具**。全程只使用「原生 git / svn 命令 + 本 skill 自带 `scripts/` 脚本」。不要为完成流程去查找、加载或套用其他 skill。

> **未实测标记（SVN 部分）**：`scripts/svn_report.py` 的输出解析与 SVN 命令族尚未在真实 SVN 工作副本实测（本机无 svn CLI）。移出条件：下次遇到 SVN 项目 → 跑 SOP 第 1、2 步 → 核对脚本输出与 `svn info/status/-u/diff/log` 原始结果一致 → 删除本标记与文末「项目约定」中的对应说明。

## 一、固定步骤（SOP，机械执行）

> 每个步骤只执行其规定的一条动作；未到某步骤，禁止预跑其后步骤；不做 SOP 之外的动作。

### A. 提交（commit）
1. **探测 VCS**：跑 `git rev-parse --is-inside-work-tree`。true→Git；false→再跑 `svn info --non-interactive`，成功→SVN；两者都失败或都成功→停下问用户用哪个。
2. **巡检**：跑 `python .codebuddy/skills/version-control/scripts/git_report.py`（Git）或 `svn_report.py`（SVN）。不运行任何其他命令。
3. **出提案**：严格套用下方「提案模板」逐字填，不额外分析。
4. **停下等确认**：只有「确认 <编号>」才继续。
5. **暂存并提交**：
   - Git：`git add -- <path...>` → 用 write_to_file 写 `.codebuddy/tmp-commit-msg.txt`（UTF-8）→ `git commit -F .codebuddy/tmp-commit-msg.txt` → 删除临时文件。
   - SVN：先 `svn update`，再 `svn commit -m "<信息>" <path...>`（禁止不带路径）。
6. **提醒推送**：提交后一句话列出待推送提交并问是否推送，停下。

### B. 推送（Git）
1. 查上游：`git rev-parse --abbrev-ref --symbolic-full-name @{upstream}`。
2. 有→提案 `git push`；无→提案 `git push -u origin <branch>`。等确认后执行。（SVN 无推送，改为提议 `svn update`。）

### C. 发版 / tag
1. 读 `package.json` 的 `version`，按区间提交类型推版本号：`feat`→MINOR、`fix`/`perf`→PATCH、`BREAKING CHANGE`→MAJOR、仅 `docs/style/chore/refactor`→不发版。
2. 出提案（建议版本号 + CHANGELOG 草稿：新增/修复/优化 分类）。
3. 等确认 → 打 tag（`git tag -a vX.Y.Z -m "<摘要>"`；SVN `svn copy <trunk> <tags>/vX.Y.Z -m`）→ 等确认 → 推 tag（`git push origin vX.Y.Z`）。禁止擅自改 `package.json` 版本号。

## 二、必要思考 vs 禁止思考

| 允许（仅此） | 禁止 |
|------------|------|
| 判定 VCS（一条命令） | 重新设计 / 质疑流程 |
| 读巡检输出 | 补充额外建议、说明、风险以外的业务分析 |
| 识别风险：敏感文件、冲突、detached HEAD、上游缺失、跨分支误推 | 讨论「为什么这样改」、复述规则 |
| 生成提交信息（1 推荐 + 1~2 备选，每条一行侧重点） | 执行 SOP 未列出的命令或检查 |
| — | 预跑后续阶段、连续执行未经确认的写操作 |

## 三、铁律

1. **只读先于写入**；写操作前必出提案并等确认。
2. **明确同意才算同意**：仅「确认/同意/可以/继续/OK/是」算数；沉默、提问、改口都不算，改口则重新出提案。
3. **禁止清单**（用户逐条点名也需提案确认）：
   - Git：`add .`/`-A`/`-u`、`commit -a`、`--amend`、`rebase`、`reset --hard`、`checkout .`/`restore .`、`clean -f`、`push --force`、`git config` 写入、`stash drop/clear`、`--no-verify`
   - SVN：`commit` 不带路径、`add <目录>`、`revert --depth infinity`、`delete/move/propset/propdel`、`merge`、`lock/unlock --force`、`cleanup --remove-unversioned`、`switch/relocation`、`upgrade`
4. **异常即停**：冲突、detached HEAD、上游缺失、SVN out-of-date/树冲突、未跟踪敏感文件（`.env`/`*.pem`/`*key*`）→ 报告并停。
5. **不替用户改历史**：写错只列方案（`--amend`[仅未推送]/`revert`/`reset --soft`；SVN 反向合并）由用户选。

## 四、提案模板（提交时逐字套用）

输出为**连续内容、不加任何小标题**；顺序固定：文件清单 → 提交信息建议 → 修改内容；三段之间用 `---` 分隔。

```
- [ ] <file1>（+n/-m）
- [ ] <file2>

<Git·分支 dev / SVN·URL+revision> · 本次仅提交，不推送

---

- A（推荐）`<type>(<模块>): <简述>`
  侧重点：<一句话，≤10 字>
- B `…`
  侧重点：…
- C `…`
  侧重点：…

---

<file1>（第 x-y 行，<哪个函数/表单/模板>）
（此处粘贴该文件的关键 diff：改动行 + 3 行上下文；只给 diff，不贴改动后的完整代码块；超 100 行按逻辑分段全部列出）
改前→改后：<一句>；影响：<一句>；疑点：<仅当确有可疑，否则省略>

---
<file2>（…）
…
```

强制规则：
- 只列**会提交**的文件；被忽略的文件、「不纳入本次提交」清单一律不写（敏感文件除外，见下）
- 仅当存在未版本化敏感文件时，加一行「以下文件不会被提交，请勿 add」
- 候选 2~3 条、标注推荐、每条附一行侧重点；禁止用 diffstat 数字或「调整了校验规则」「优化若干逻辑」「修改 x 个文件」等模糊概述
- **每轮必须完整展示、禁止任何省略**（不得出现「内容未变」「简述如下」「同上」「（略）」）；内容与上一轮相同也要完整重发

末尾固定一句：

```
回复「确认 A」（或 B/C、自定文本）即执行 git add + git commit，完成后提醒是否推送。
```

## 五、命令与踩坑速查

- **中文提交信息**（Windows PowerShell 5.1）：`-m "中文"` 会乱码/报「字符串缺少终止符」，必须走 `git commit -F .codebuddy/tmp-commit-msg.txt`（write_to_file 写 UTF-8，提交后删除）。
- **命令写法**：路径不加引号、命令间用分号分隔；`git add -- "路径";` 的「引号+分号」混写会被解析成带分号的 pathspec 导致 add 失败。
- 命令级 Git↔SVN 差异见 `references/vcs-mapping.md`。

## 六、项目约定（本仓库）

- Git 仓库：`origin` → `https://gitlab.hzlinks.net/zyck/meta-kitchen-web.git`；分支 `prod`（发布/集成）与 `dev`，默认未配上游，首次推送需 `-u origin <branch>`；推送前核对当前分支，禁止跨分支误推。
- 提交信息：`<type>(<模块>): <中文简述>`（类型见 `references/commit-message.md`）。
- 一次提交只做一件事；跨模块改动拆分并逐一征得用户同意。

## 资源

- `scripts/git_report.py` / `scripts/svn_report.py`：只读巡检（分支/上游/文件清单/改动统计/最近提交/tag/敏感文件/版本号）。
- `references/commit-message.md`：类型前缀表与生成规则。
- `references/release-versioning.md`：版本号推导、tag 与 CHANGELOG 模板。
- `references/vcs-mapping.md`：Git↔SVN 命令映射与禁止清单。
