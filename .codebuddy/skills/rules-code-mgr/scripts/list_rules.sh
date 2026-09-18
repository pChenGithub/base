#!/bin/bash
# 列出 .codebuddy/rules/ 下编码类规则并校验命名合规性（固定前缀 code、无序号）
# 用法: bash list_rules.sh [rules目录路径]   (默认 .codebuddy/rules/)

RULES_DIR="${1:-$(pwd)/.codebuddy/rules}"

if [ ! -d "$RULES_DIR" ]; then
    echo "错误: 目录不存在 $RULES_DIR"
    exit 1
fi

echo "== 规则文件列表 =="
ls -1 "$RULES_DIR"/*.mdc 2>/dev/null || { echo "(无 .mdc 文件)"; exit 0; }

echo ""
echo "== 命名合规性检查 =="
FAIL=0
for f in "$RULES_DIR"/*.mdc; do
    base=$(basename "$f")
    # 校验: 编码类必须 code_ 前缀 + 小写下划线主体 + .mdc (memory_load 桥接除外)
    if [[ "$base" == "memory_load.mdc" ]]; then
        continue
    fi
    if [[ ! "$base" =~ ^code_[a-z][a-z0-9_]*\.mdc$ ]]; then
        echo "[不合规] $base  -> 应为 code_<语言|通用主题>.mdc (固定前缀 code、无序号、小写下划线)"
        FAIL=1
        continue
    fi
    # 校验 frontmatter
    if ! grep -q '^description:' "$f"; then
        echo "[不合规] $base  -> 缺少 frontmatter description"
        FAIL=1
    fi
    # 校验个性化挂载点
    if ! grep -q '个性化扩展' "$f"; then
        echo "[提示] $base  -> 缺少个性化扩展挂载点"
    fi
done

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "检查完成: 全部合规"
else
    echo "检查完成: 存在不合规项(见上)"
fi
exit $FAIL
