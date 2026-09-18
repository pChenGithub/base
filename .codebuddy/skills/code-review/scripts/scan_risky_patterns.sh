#!/bin/sh
# ============================================================
# 高危模式扫描脚本（code-review skill 辅助工具）
#
# 用途：在指定路径上快速定位"疑似问题"位置，作为复核线索。
#      输出**不是结论**——命中项必须回到代码上下文确认后，
#      才能计入代码审查报告（见 references/问题模式库.md）。
#
# 用法：sh scan_risky_patterns.sh <路径> [扩展名...]
#   例：sh scan_risky_patterns.sh src
#       sh scan_risky_patterns.sh . c h
#       sh scan_risky_patterns.sh ../../netopt c h
#
# 说明：
#   1) 本脚本以 grep -E 实现，仅覆盖"可直接文本检出"的模式；
#      内存泄漏、竞态、off-by-one 等语义缺陷须靠 AI/人工推理。
#   2) C++ 专有模式（裸 new/delete、C 风格转换、C++14/17 特性）
#      仅在扩展名含 cpp/cc/cxx/hpp 时执行，避免在 C 文件上误报。
# ============================================================

set -u

if [ $# -lt 1 ]; then
    echo "用法: $0 <路径> [扩展名...]" >&2
    echo "  例: $0 src c h" >&2
    exit 1
fi

targetPath="$1"
shift

# 默认扫描的源代码扩展名
if [ $# -gt 0 ]; then
    extList="$*"
else
    extList="c h cpp hpp cc"
fi

# 组装 grep 的 --include 参数（每扩展名一项）
includeArgs=""
cxxIncludeArgs=""
for ext in $extList; do
    includeArgs="$includeArgs --include=*.$ext"
    case "$ext" in
        cpp|cc|cxx|hpp)
            cxxIncludeArgs="$cxxIncludeArgs --include=*.$ext"
            ;;
    esac
done

# 扫描单个模式（C/C++ 通用）：参数为 编号 / 描述 / 正则
scanPattern() {
    patternId="$1"
    patternDesc="$2"
    patternRe="$3"

    # shellcheck disable=SC2086
    hitList=$(grep -rnE $includeArgs -- "$patternRe" "$targetPath" 2>/dev/null)
    if [ -n "$hitList" ]; then
        echo ""
        echo "### [$patternId] $patternDesc"
        echo "$hitList"
    fi
}

# 扫描单个模式（仅 C++ 文件）：无 C++ 扩展名时跳过
scanPatternCxx() {
    if [ -z "$cxxIncludeArgs" ]; then
        return
    fi

    patternId="$1"
    patternDesc="$2"
    patternRe="$3"

    # shellcheck disable=SC2086
    hitList=$(grep -rnE $cxxIncludeArgs -- "$patternRe" "$targetPath" 2>/dev/null)
    if [ -n "$hitList" ]; then
        echo ""
        echo "### [$patternId] $patternDesc"
        echo "$hitList"
    fi
}

echo "============================================================"
echo "高危模式扫描（线索，非结论）"
echo "路径: $targetPath"
echo "扩展名: $extList"
echo "============================================================"

# ---- 1. 禁止项：可直接文本检出 ----
scanPattern "S01" "无边界字符串/输入函数（C 禁止 strcpy/strcat/sprintf/scanf）" \
    '\b(strcpy|strcat|sprintf|vsprintf|gets|scanf|sscanf)\s*\('
scanPattern "S02" "#pragma once（禁止，须用 include guard）" \
    '#[[:space:]]*pragma[[:space:]]+once'
scanPattern "S03" "goto（仅允许错误处理清理，须逐条复核）" \
    '\bgoto\b'
scanPattern "S04" "VLA / alloca（禁止动态栈分配）" \
    '\balloca\s*\('
scanPattern "S05" "C 文件中的 // 注释（C89 不兼容，须 /* */）" \
    '(^[[:space:]]*//|[;{}][[:space:]]*//)'
scanPattern "S06" "for/while 条件含 <=（疑似 1 起始下标，P0-2-1）" \
    '(for|while)[[:space:]]*\([^)]*<='
scanPattern "S07" "system / popen / exec（命令执行，安全与注入风险）" \
    '\b(system|popen|execl|execv|execve)[[:space:]]*\('

# ---- 2. C++ 专有禁止项（仅在含 C++ 扩展名时扫描）----
scanPatternCxx "S08" "裸 new / delete（C++ 禁止，须 RAII/智能指针）" \
    '\b(new|delete)\b'
scanPatternCxx "S09" "C 风格强制转换（C++ 禁止，须 static_cast 等；(void) 空转换已排除）" \
    '\([[:space:]]*(unsigned[[:space:]]+)?(int|char|long|short|float|double|size_t|u8|u16|u32|s8|s16|s32)[[:space:]]*\**[[:space:]]*\)|\([[:space:]]*void[[:space:]]*\*+[[:space:]]*\)'
scanPatternCxx "S10" "C++14/17 禁用特性（GCC 4.7 / C++11）" \
    '(make_unique|std::optional|std::variant|if[[:space:]]+constexpr|\[\[maybe_unused\]\]|std::cbegin|std::cend)'

# ---- 3. 高危调用：需核对释放/返回值 ----
scanPattern "S11" "堆分配调用（须核对 free 配对与判空）" \
    '\b(malloc|calloc|realloc|strdup|strndup)[[:space:]]*\('
scanPattern "S12" "直接使用 realloc 返回值覆盖原指针（失败即泄漏）" \
    '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*realloc[[:space:]]*\('
scanPattern "S13" "睡眠/延时调用（中断或实时上下文不安全）" \
    '\b(sleep|usleep|msleep|nanosleep|delay)[[:space:]]*\('
scanPattern "S14" "格式化输出（须核对格式串与参数类型匹配）" \
    '\b(printf|fprintf|snprintf|sprintf|LOG_I|LOG_E|LOG_D)[[:space:]]*\('
scanPattern "S15" "文件/目录/套接字资源（须核对 close/closedir）" \
    '\b(open|fopen|opendir|socket|accept|pipe|inotify_init)[[:space:]]*\('
scanPattern "S16" "锁与信号量（须核对配对与错误路径释放）" \
    '\b(pthread_mutex_lock|pthread_mutex_unlock|sem_wait|sem_post|sem_init|sem_destroy)[[:space:]]*\('
scanPattern "S17" "线程创建（须核对 join/detach）" \
    '\bpthread_create[[:space:]]*\('
scanPattern "S18" "memcpy/memset/memmove（须核对长度与目标容量）" \
    '\b(memcpy|memset|memmove|strncpy|strncat)[[:space:]]*\('

# ---- 4. 待办与未完成标记 ----
scanPattern "S19" "TODO / FIXME / XXX 标记（未完成事项）" \
    '(TODO|FIXME|XXX|HACK)'

echo ""
echo "============================================================"
echo "扫描结束。以上均为【线索】，须逐条回代码上下文复核后再定级。"
echo "============================================================"
