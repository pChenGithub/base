# 版本号、tag 与 CHANGELOG 规范

## 语义化版本（SemVer）

格式 `MAJOR.MINOR.PATCH`，tag 名称统一加 `v` 前缀：`v1.2.0`。

| 变更类型 | 版本位 | 判定依据 |
|---------|-------|---------|
| 破坏性变更 | MAJOR | 存在 `BREAKING CHANGE:`，或接口字段/入参不兼容调整 |
| 新功能 | MINOR | 存在 `feat` 提交 |
| 缺陷修复 | PATCH | 仅有 `fix` / `perf` 提交 |
| 无实质变化 | 不发版 | 仅有 `docs` / `style` / `chore` / `refactor` 提交 |

版本号起始值取 `package.json` 的 `version` 字段。若 `package.json` 与最近 tag 不一致，**必须先向用户报告不一致并询问以哪个为准**。

## 推导流程

1. 读取 `package.json` → 当前版本
2. 列出最近 tag 到 HEAD 的提交：`git log --oneline <last-tag>..HEAD`（无 tag 时用全量最近 N 条）
3. 按上表统计类型，得出建议版本位
4. 输出提案并停下等待用户确认

## 提案模板

```
## 版本提案（待确认）
- 当前版本：1.0.0（package.json）/ 最近 tag：v1.0.0
- 区间提交：
  - fix(外部授权): 仅待处理和已接受状态可撤回
  - feat(租户管理): 管理员信息改为选填可编辑并新增邮箱校验
- 判定：包含 feat → 建议 MINOR
- 建议版本：v1.1.0
- tag 名称：v1.1.0
```

## CHANGELOG 草稿模板

```
## v1.1.0（2026-09-16）

### 新增
- 租户管理：管理员信息改为选填可编辑，新增邮箱格式校验

### 修复
- 外部授权：仅「待处理」「已接受」状态可执行撤回

### 优化
- 字典管理：删除确认框层级调整
```

分类与提交类型的映射：`feat` → 新增，`fix` / `perf` → 修复，`refactor` / `style` → 优化，`chore` / `docs` / `test` → 其他。

## 执行约束

- **Git**：tag 必须使用注解标签（`git tag -a`），信息取自用户确认的 CHANGELOG 摘要
- **SVN**：tag 由服务端目录复制生成（`svn copy <trunk-URL> <tags-URL>/vX.Y.Z -m "<摘要>"`）；执行前必须先与用户确认 trunk / tags 的实际 URL 布局，复制操作立即在服务器生效，需单独确认
- 打 tag 与推送 tag（Git）/ 复制 tag（SVN）是**独立确认点**，不得合并为一次确认后连续执行
- 禁止在用户未确认版本号前修改 `package.json` 的 `version`
- 禁止删除或移动已推送的 tag（`git tag -d` / `git push --delete`）除非用户明确点名要求并确认风险
- 禁止 `git push --force` 覆盖远程 tag
