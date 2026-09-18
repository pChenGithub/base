# Git ↔ SVN 命令映射与语义差异

> 执行任何命令前先核对本表。**SVN 模式的确认强度必须高于 Git 模式**：SVN 提交直接落服务器，没有本地回滚点。

## 一、逐阶段映射

| 阶段 | Git 命令 | SVN 命令 | 语义差异与注意 |
|------|---------|---------|---------------|
| VCS 探测 | `git rev-parse --is-inside-work-tree` | `svn info --non-interactive` | 两者都成功时（git-svn / 双工作副本）必须让用户指定，禁止自行选择 |
| 1 巡检 | `git status --porcelain=v1 --branch`、`git diff --stat`、`git diff --cached --stat`、`git log --oneline -N` | `svn status -u`、`svn status`、`svn diff`、`svn log -l N`、`svn info` | SVN 用 `-u` 检查服务端是否有新版本（`*` 标记）；无"领先/落后提交数"概念，只有 working revision 与 HEAD revision |
| 1 未跟踪文件 | `git status` 的 `??` | `svn status` 的 `?` | 两侧都要做敏感文件告警（`.env`、`*.pem`、`*key*` 等） |
| 1 忽略规则 | 读 `.gitignore` | `svn propget svn:ignore <dir>` | SVN 忽略是目录属性；`.gitignore` 不存在 |
| 2 圈定提交范围 | `git add -- <path1> <path2>`（写入索引区） | **无索引区**：`svn changelist <变更列表名> <path...>` 做分组，或跳过本阶段、在阶段 4 直接 `svn commit <path...>` | Git 的 `add` 是"暂存"；SVN 的 changelist 仅做本地分组标记，不影响服务端 |
| 2 取消选择 | `git restore --staged <path>` | `svn changelist --remove <path>` | SVN 不存在"取消暂存内容"的概念 |
| 3 提交信息 | 同 | 同 | 两侧都遵循 `references/commit-message.md`；SVN 无客户端 commitlint 钩子，规范完全靠确认流程保障 |
| 4 提交前置 | 无需（本地提交） | **必须先 `svn update`** | 先更新再提交，避免 out-of-date（E155011 / E160028） |
| 4 提交 | `git commit -m "<msg>"`（本地，可 `--amend`） | `svn commit -m "<msg>" <path...>`（**直接写服务器**） | SVN 无 `--amend`，写错只能 `svn merge -c -R<rev>` 反向合并或再提交修正 |
| 4 提交后留痕 | `git show --stat HEAD` | 记录返回的 revision 号（`svn info` / `svn log -l 1`），写进汇报 | revision 号是后续撤销的唯一依据，必须回显给用户 |
| 5 推送 | `git push` / `git push -u origin <branch>` | **不存在推送**，已由阶段 4 完成；改为 `svn update` 并汇报 | Git 在阶段 5 才对外可见；SVN 在阶段 4 已对外可见 |
| 5 分支 | `git switch/checkout <branch>`（本地分支） | `svn switch <URL>`（切换工作副本指向） | SVN 分支是服务端目录，切分支会改动整个工作副本，禁止擅自执行 |
| 6 版本号 | 读 `package.json` version | 同 | 两侧都禁止擅自修改 `package.json` |
| 6 tag | `git tag -a vX.Y.Z -m "<msg>"` + `git push origin vX.Y.Z` | `svn copy <trunk-URL> <tags-URL>/vX.Y.Z -m "<msg>"`（**服务端复制，立即生效**） | SVN 无推送步骤但操作即刻上服务器，故仍需二次确认；tag 目录布局（`trunk/branches/tags`）必须先向用户确认 |
| 6 查看 tag | `git tag --sort=-v:refname` | `svn list <repo-root>/tags` | 需先确认仓库根 URL，`^/tags` 依赖工作副本的根 |

## 二、SVN 下的草稿与还原

| 目的 | Git | SVN |
|------|-----|-----|
| 暂存草稿 | `git stash` | `svn diff > backup.patch`（存到仓库外或已被忽略的目录） |
| 恢复草稿 | `git stash pop` | `svn patch backup.patch` |
| 还原单个文件 | `git restore <path>` | `svn revert <path>` |
| 还原全部（危险） | `git restore .` | `svn revert --depth infinity .` |

上述 SVN 命令中，除 `svn diff` 外全部属于写操作，必须走提案确认。

## 三、SVN 特有陷阱（发现即停并报告）

1. **out-of-date**：提交报 `E155011 / E160028`，说明服务端有新版本，先 `svn update` 再重新确认提交。
2. **树冲突（Tree conflict）**：`svn status` 出现 `C`，先报告冲突文件与类型，由用户决定 `svn resolve --accept=working` 或手工处理。
3. **mixed-revision**：工作副本文件处于不同 revision（常态，但提交前需说明影响范围）。
4. **`svn:externals`**：`svn status` 出现 `X`，表示外部引用的其他仓库，提交时不会包含其内容，需向用户说明。
5. **文件锁**：状态含 `L`（已加锁）或存在 `svn:needs-lock` 属性，改动前需确认是否要加锁。
6. **未版本化敏感文件**：`?` 状态中出现 `.env` / `*.pem` / `*key*` 时，禁止 `svn add`，并给出加入 `svn:ignore` 的建议（需用户确认）。
7. **`svn add` 的递归性**：`svn add <dir>` 会递归纳入目录下所有未版本化文件，等同于 Git 的 `git add .`，**属于禁止操作**，必须逐个文件指定。

## 四、禁止清单（SVN）

- `svn commit` 不带路径列表（等于提交整个工作副本的改动）
- `svn add <目录>`（递归纳入，等同 `git add .`）
- `svn revert --depth infinity`（丢弃全部本地改动，不可恢复）
- `svn delete` / `svn move` / `svn propset` / `svn propdel`（改变版本库结构或元数据）
- `svn merge` / `svn merge -c -R<rev>`（反向合并撤销必须由用户点名并要求，且需确认已推送 revision 的影响范围）
- `svn lock` / `svn unlock --force`
- `svn cleanup --remove-unversioned` / `--remove-ignored`（删除未版本化文件）
- `svn switch` / `svn relocation`（改变工作副本指向）
- `svn upgrade`（升级工作副本格式，不可回退）
- `--force`、`--no-auth-cache` 之外的认证类参数改写
- 任何直接修改 `.svn/` 目录内容的行为

## 五、禁止清单（Git，与 SKILL.md 一致）

- `git add .` / `-A` / `-u` / `git commit -a`
- `git commit --amend`、`git rebase`、`git filter-branch`
- `git reset --hard`、`git checkout .`、`git restore .`、`git clean -f`
- `git push --force` / `-f`（尤其推往 `main` / `master` / `prod`）
- `--no-verify` / `--no-gpg-sign` 等跳过校验开关
- `git config` 任何写入、`git stash drop` / `git stash clear`
