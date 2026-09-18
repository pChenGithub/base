# 编码类 rules 完整示例（多语言）

> 示例仅作结构示范；实际生成时按目标项目技术栈填充内容。本项目（ZYACS_Dctr_328）为 Qt4 C++ 嵌入式项目，C++ 示例可直接参考。

## 示例 1：C++（适配本项目）

文件：`code_cpp.mdc`

```markdown
---
description: C++ 编码规范：语言版本特性、注释风格、禁止项与正反示例
globs: "**/*.{cpp,h,hpp,cc}"
---

# C++ 编码规范

## 适用范围

本项目全部 C++ 源码（.cpp/.h/.hpp/.cc），含 Qt Widgets 上层与 C 混编外层封装。

## 编程语言

- 语言：C++
- 语言版本特性：编译选项 `-std=c++0x`（C++11）；C++11 通用特性 AI 已知，不赘述

## 注释风格

- 文件头：`/** 文件用途、作者、日期 */`
- 函数头：`/** @brief 功能 @param 入参 @return 返回值 */`
- 行内：中文 `//`，解释"为什么"

## 禁止项

- 禁止 `using namespace std;`
- 禁止头文件定义非 inline 全局对象
- 禁止魔法数字裸用

## 正向示例

```cpp
// 正确：具名常量 + 边界检查
void openDoor(int doorId) {
    const int kMaxDoors = 8;
    if (doorId >= 0 && doorId < kMaxDoors) {
        relayOn(doorId);
    }
}
```

## 反向示例

```cpp
// 错误：using namespace + 魔法数字
using namespace std;
void openDoor(int a) {
    if (a >= 0 && a < 8) { relayOn(a); }
}
```

## 个性化扩展

> 本单元为扩展挂载点：可根据项目/用户需要自由追加模块化小节，不破坏既有结构。
> 追加时保持小节独立、命名清晰、编号递增（## 个性化扩展 1、## 个性化扩展 2 …）。

### 个性化扩展 1
<!-- 本项目约定：底层 C 实现 + 上层 C++/Qt 封装，extern "C" 混编；全局数据 g_WorkInfo/g_LocalInfo 见 global.h -->
```

## 示例 2：C（底层库风格）

文件：`code_c.mdc`

```markdown
---
description: C 编码规范：面向嵌入式底层库
globs: "**/*.{c,h}"
---

# C 编码规范

## 适用范围

base/、protocol/rs485 等 C 底层库。

## 编程语言

- 语言：C
- 语言版本特性：项目兼容 C89/C99；影响编码的版本差异（C89 不支持 `//` 注释、C99 支持 VLA）已落到禁止项

## 注释风格

- 函数头：`/** @brief ... @param ... @return ... */`
- 块注释说明算法，行内注释说明边界

## 禁止项

- 禁止 `//` 注释（C89 不兼容，统一 `/* */`）
- 禁止可变长数组
- 禁止隐式指针转换

## 正向示例

```c
/* 正确：显式判空 + 固定长度 */
int copyId(char *dst, const char *src, size_t len) {
    if (dst == NULL || src == NULL) { return -1; }
    strncpy(dst, src, len);
    return 0;
}
```

## 反向示例

```c
/* 错误：不判空 + 隐患拷贝 */
int copyId(char *dst, const char *src) {
    strcpy(dst, src);   /* 错误：长度未知 */
    return 0;
}
```

## 个性化扩展

> 本单元为扩展挂载点：可根据项目/用户需要自由追加模块化小节，不破坏既有结构。
> 追加时保持小节独立、命名清晰、编号递增（## 个性化扩展 1、## 个性化扩展 2 …）。

### 个性化扩展 1
<!-- 追加内容... -->
```

## 示例 3：Python

文件：`code_python.mdc`（结构同前，语言版本特性按需记录：仅写影响编码的版本差异，如 Python 3.8+ 海象运算符，AI 已知则不赘述；注释风格中文，禁止项如 `import *` 等）

## 示例 4：跨语言通用文件（通则）

文件：`code_comment.mdc`

结构同模板，但 **`## 编程语言` 单元改为"适用语言"**（列出所有适用语言），正文只放跨语言通则；各语言特例留在对应语言文件（如 `code_cpp.mdc` 注释风格单元）中，并在此引用语言文件。

## 使用说明

1. 按 Step 1 拆分决策：一个语言一个文件（`code_<语言>.mdc`）；仅跨语言通用主题独立成文件（`code_<主题>.mdc`）
2. 文件主体（语言/主题）避免与已有文件冲突
3. 正反示例必须与禁止项一一对应（示例即约束的落地）
4. 尾部保留个性化挂载点，项目专属约定写入挂载点小节而非改动必备单元
