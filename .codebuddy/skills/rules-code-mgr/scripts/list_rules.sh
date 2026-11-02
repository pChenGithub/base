#!/bin/bash
# 列出 .codebuddy/rules/ 下编码类规则并校验：
#   1) 文件命名（固定前缀 code、无序号、小写下划线主体）
#   2) frontmatter（description 必填且不过长 / globs 提示）
#   3) 必备单元（适用范围 / 注释风格 / 禁止项 / 正向示例 / 反向示例）
#   4) 个性化扩展挂载点（提示）
#   5) 内容客观性（禁止绑定具体项目 / 业务：项目名、业务模块、业务对象、型号、项目全局量、绝对路径）
#   6) 同文件重复条文（不合规）；跨文件重复条文（提示，应收敛为一处 + 指针）
#
# 用法: bash list_rules.sh [rules目录路径]   (默认 <当前目录>/.codebuddy/rules/)
# 退出码: 0=全部合规；1=存在不合规项

RULES_DIR="${1:-$(pwd)/.codebuddy/rules}"

if [ ! -d "$RULES_DIR" ]; then
    echo "错误: 目录不存在 $RULES_DIR"
    exit 1
fi

# 项目/业务标志词（命中即不合规）：规范只做客观描述，不得涉及具体项目或具体业务。
# 维护方式：新增项目/业务特有名词时在此追加，保持为 POSIX ERE。
TERMS='zyck|SmartLink|qxosui|myapp|X-AIOS|ECP|scbox|SCBOX|Scbox|g_globalData|app_config|dataMeta|YC[0-9]{4}|DDSU|DTSU|templimit|TempLimit|Nfc|继电器|电表|智控盒|档位|温度|告警|安规|息屏|场景联动|/home/|/opt/'

# description 建议上限（字符数，中文按 1 字符计）
DESC_MAX=120

SCAN_FILE=$(mktemp)
trap 'rm -f "$SCAN_FILE"' EXIT

echo "== 规则文件列表 =="
ls -1 "$RULES_DIR"/*.mdc 2>/dev/null || { echo "(无 .mdc 文件)"; exit 0; }

echo ""
echo "== 结构 / 客观性 / 冗余检查 =="
FAIL=0
for f in "$RULES_DIR"/*.mdc; do
    base=$(basename "$f")

    # memory_load.mdc 属加载桥接（官方机制），不在本 skill 范围
    if [[ "$base" == "memory_load.mdc" ]]; then
        continue
    fi

    # 1) 文件名：code_<语言|通用主题>.mdc
    if [[ ! "$base" =~ ^code_[a-z][a-z0-9_]*\.mdc$ ]]; then
        echo "[不合规] $base  -> 应为 code_<语言|通用主题>.mdc (固定前缀 code、无序号、小写下划线)"
        FAIL=1
        continue
    fi

    # 2) frontmatter
    if ! grep -q '^description:' "$f"; then
        echo "[不合规] $base  -> 缺少 frontmatter description"
        FAIL=1
    else
        desc=$(grep -m1 '^description:' "$f" | sed 's/^description:[[:space:]]*//')
        desc_len=$(printf '%s' "$desc" | wc -m)
        if [ "$desc_len" -gt "$DESC_MAX" ]; then
            echo "[提示] $base  -> description 过长($desc_len 字符 > $DESC_MAX)，应一句话精确描述"
        fi
    fi
    if ! grep -q '^globs:' "$f"; then
        echo "[提示] $base  -> 未声明 globs（规则全项目适用时可省略）"
    fi

    # 3) 必备单元
    for sec in 适用范围 注释风格 禁止项 正向示例 反向示例; do
        if ! grep -q "^## $sec" "$f"; then
            echo "[不合规] $base  -> 缺少必备单元「$sec」"
            FAIL=1
        fi
    done

    # 4) 个性化扩展挂载点
    if ! grep -q '个性化扩展' "$f"; then
        echo "[提示] $base  -> 缺少个性化扩展挂载点"
    fi

    # 正文起点：跳过 frontmatter
    body_start=$(awk 'BEGIN{c=0} /^---[[:space:]]*$/{c++; if(c==2){print NR+1; exit}}' "$f")
    body_start=${body_start:-1}

    # 5) 内容客观性：逐行匹配项目/业务标志词（跳过 globs 行与代码块围栏）
    hits=$(awk -v start="$body_start" -v terms="$TERMS" '
        NR >= start && $0 !~ /^globs:/ && $0 !~ /^```/ {
            n = split(terms, arr, "|")
            for (i = 1; i <= n; i++) {
                if ($0 ~ arr[i]) {
                    printf "%d:%s\n", NR, arr[i]
                }
            }
        }' "$f" | sort -u)
    if [ -n "$hits" ]; then
        while IFS=: read -r ln term; do
            echo "[不合规] $base:$ln -> 命中项目/业务词「$term」（规范只做客观描述，不得涉及具体项目或具体业务）"
        done <<< "$hits"
        FAIL=1
    fi

    # 6) 同文件重复条文：代码块外的 "- " 条文行，重复即视为冗余
    dups=$(awk -v start="$body_start" '
        BEGIN { fence = 0 }
        NR >= start {
            if ($0 ~ /^```/) { fence = !fence; next }
            if (fence) next
            if ($0 !~ /^- /) next
            line = $0
            sub(/[ \t]+$/, "", line)
            if (length(line) < 12) next
            cnt[line]++
            if (!(line in first)) first[line] = NR
        }
        END {
            for (l in cnt) {
                if (cnt[l] > 1) printf "%d\t%d\t%s\n", first[l], cnt[l], substr(l, 1, 40)
            }
        }' "$f")
    if [ -n "$dups" ]; then
        while IFS=$'\t' read -r ln times text; do
            echo "[不合规] $base:$ln -> 同文件重复条文（${times} 次）：$text…（应合并为一条）"
        done <<< "$dups"
        FAIL=1
    fi

    # 收集条文用于跨文件重复提示
    awk -v f="$base" -v start="$body_start" '
        BEGIN { fence = 0 }
        NR >= start {
            if ($0 ~ /^```/) { fence = !fence; next }
            if (fence) next
            if ($0 !~ /^- /) next
            line = $0
            sub(/[ \t]+$/, "", line)
            if (length(line) < 12) next
            printf "%s\t%s\n", f, line
        }' "$f" >> "$SCAN_FILE"
done

# 7) 跨文件重复条文（提示）：同一纪律只应在一处成条，另一处留「判据 + 指针」
cross=$(awk -F'\t' '
    {
        if ($2 ~ /个性化扩展|本单元为扩展挂载点|追加时保持小节独立/) next
        if (!($2 in files)) {
            files[$2] = $1
        } else if (index(files[$2], $1) == 0) {
            files[$2] = files[$2] "," $1
        }
    }
    END {
        for (l in files) {
            if (index(files[l], ",") > 0) printf "%s :: %s\n", files[l], substr(l, 1, 50)
        }
    }' "$SCAN_FILE" | sort)
if [ -n "$cross" ]; then
    echo "[提示] 跨文件重复条文（应收敛为一处成条 + 其余只留指针）："
    echo "$cross" | sed 's/^/    /'
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "检查完成: 全部合规"
else
    echo "检查完成: 存在不合规项(见上)"
fi
exit $FAIL
