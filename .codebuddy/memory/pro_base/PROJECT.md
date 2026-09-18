# base 项目描述

- 更新日期: 2026-09-18
- 状态: 已初始化（基于代码仓库提炼四要素 + 用户确立项目原则）

## 一、项目概述

- **项目名称**：base（base lib and third lib）
- **定位**：基于 C 开发的基础库集合，向上层（宿主）工程提供自研公共接口，并封装/收纳所需第三方 C 库。
- **形态**：非可独立运行的应用，而是 **qmake 的 `.pri` 片段库 + 第三方源码/预编译库集合**。上层工程通过 `include(base.pri)` 引入，并用 `DEFINES += M_XXX` 按需裁剪编入的功能模块。
- **主要功能范围**：
  - 自研基础能力：字符串/编码转换、文件与目录操作、时间操作、串口、网络配置（IP/网关/MAC/DHCP/netlink/DNS/WiFi）、HTTP 客户端、MQTT 客户端、Web 服务器封装、日志、JSON 封装、双向链表、shell 执行、存储（MMC）容量统计、崩溃转储等。
  - 第三方库收纳：libcurl、OpenSSL、paho.mqtt.c、libmodbus、mongoose、zlog、zint、uthash、zlib、libpng、wpa_supplicant（裁剪）。
- **宿主依赖约定**：多个源文件 `#include "global.h"`，但仓库内不提供 `global.h`（由宿主工程提供）——即本库需宿主工程配套编译环境。

## 二、项目技术栈

### 语言与构建

| 项 | 内容 |
|---|---|
| 主语言 | C |
| 构建系统 | qmake `.pri` include 片段（`base.pri`、`file/file.pri`、`json/json.pri`、`string/string.pri`），无顶层 `.pro` 工程文件 |
| 裁剪机制 | `DEFINES` 宏条件编译（`contains (DEFINES, M_XXX)` 块）+ `LIBS`/`INCLUDEPATH` 关联第三方库 |
| 编译目标 | aarch64（`gcc-linaro-10.2.1-2021.01-x86_64_aarch64-linux-gnu`）、arm-linux-gnueabihf（devkit / A8）、x86 |

### 第三方库与版本

| 库 | 版本 | 仓库内形态 |
|---|---|---|
| libcurl | 7.85.0 | 头文件完整 + 预编译 `curl/lib/libcurl.so.4.8.0` |
| OpenSSL | 1.1.1e | 仅构建记录（`build.txt`），产物 `buid_aarch64/` 被 gitignore；aarch64 交叉编译（`no-asm no-async`） |
| paho.mqtt.c | 1.3.9 | 头文件完整 + `mqtt/lib` 预编译（`-lpaho-mqtt3c`）；另有 mosquitto 2.0.15 构建记录 |
| libmodbus | 3.1.11 | 全量源码（`modbus-version.h`） |
| mongoose | 无版本文件 | 源码内联（`mongoose.c/.h`） |
| zlog | 1.2.18 | 源码 `zlog-1.2.18/` + 预编译 `out_x86/`、`out_devkit/`（`libzlog.so.1.2.12`、`libzlog.a`） |
| zint | 2.13.0 | 全量源码（条码/二维码生成） |
| uthash | 无版本 | 单头 `include/uthash.h` |
| zlib | 官网 zlib.net | 仅构建记录，产物被 gitignore |
| libpng | 1.6.45（`source_code.txt` 指向；`.gitignore` 另含 1.4.22 痕迹） | 仅 `img/png/` 下构建记录，源码/产物被 gitignore |
| wpa_supplicant | 2.10 | 仅裁剪 `wpas/`（`wpa_ctrl.c/.h`、`os_unix.c` 等） |

### 第三方模块管理三件套（统一约定）

各第三方子目录统一以三个文件管理：`source_code.txt`（上游来源/地址）、`build.txt`（编译命令与工具链参数）、`.gitignore`（屏蔽源码与产物）。

## 三、项目架构

### 总体分层

仓库为「能力模块集合」：每个顶层目录是一个独立功能模块，自带 `.h/.c` 与错误码头文件 `*_errno.h`（或 `*Err.h`），由 `base.pri` 通过 `DEFINES` 宏选择性编入宿主工程。模块之间尽量单一职责、互不交叉。

### base.pri 条件编译映射

| 宏 | 编入源文件 | 头文件 / 包含路径 | 附加 LIBS |
|---|---|---|---|
| `M_MONGOOSE` | `mongoose/mongoose.c`、`mongoose/webserver.c` | `mongoose.h`、`webserver.h`、`web_config.h` | — |
| `M_CJSON` | `json/cJSON.c`、`json/cJSON_tools.c` | `cJSON.h`、`cJSON_tools.h`、`json_errno.h` | — |
| `M_MQTT` | `mqtt/mqttPahocClient.c`（C++ `taskMqttBase.cpp` 已注释停用） | `mqttClient.h`、`mqttPahocClient.h`；`mqtt`、`mqtt/include` | `-Lmqtt/lib -lpaho-mqtt3c` |
| `M_MIXSHELL` | `misc/mixShell.c` | `mixShell.h` | — |
| `M_DEBUG` | `misc/debug_opt.c` | `debug_opt.h` | — |
| `M_STRING` | `string/string_opt.c` | `string_opt.h` | — |
| `M_FILE` | `file/file_opt.c` | `file_opt.h` | — |
| `M_TIME` | `timeopt/timeopt.c` | `timeopt.h` | — |
| `M_HTTPC` | `curl/ifaceHttpReq.c` | `ifaceHttpReq.h`；`curl`、`curl/include` | `-Lcurl/lib -lcurl`；`-Lopenssl/buid_aarch64/lib -lssl -lcrypto` |
| `M_LOG` | `zlog/lib_log.c`、`zlog/lib_logTool.c` | `lib_log.h`、`lib_logTool.h` | `-Lzlog/lib -lzlog` |
| `M_NET` | `netopt/net_opt.c`、`net_dhcp.c`、`net_netlink.c` | `net_opt.h`、`net_dhcp.h`、`net_netlink.h` | — |
| `M_MMC` | `misc/mmc_opt.c` | `mmc_opt.h` | — |

### 未纳入 base.pri 的模块/文件（现状事实）

- 目录级：`dataStruct/linkedList/`、`modbus/`、`uart/`、`zint/`、`img/`、`include/uthash.h` 未出现在任何 `DEFINES` 块中。
- 文件级：`misc/mac_opt.c`、`misc/rand_ops.c`、`netopt/net_dns.c`、`netopt/wifi_opt.c`、`netopt/wifi_opt_slf.c`、`curl/zyEcpHttpReq.c` 未被 `base.pri` 引用。
- 备注：`base.pri` 中 `M_MMC` 块上方注释误写为「# net opt」（实际编入 `misc/mmc_opt.c`）。

### 数据流与设计模式

- **统一错误码**：各模块头文件定义 `XXXERR_*` 宏，接口以负错误码返回（如 `-MIXSHELLERR_POPEN_FAIL`、`HTTPERROR_*`、`NETERR_*`、`MQERR_*`）。
- **回调驱动**：网络事件、DHCP 状态、串口收发、MQTT 消息、HTTP 响应、shell 逐行输出均以函数指针回调（`hand_netlink_ev`、`hand_dhcpstatus`、`hand_uart_msg`、`handleMessage`、`HTTP_REPLY_CONTENT_FUNC`、`shell_ret_line`）。
- **C 语言面向对象封装（策略模式）**：`mqttClient.h` 定义抽象接口 `MQClient`（含 `MQTYPE type`）；`mqttPahocClient.h` 以 `MQClientPahoC` 首成员 `MQClient base` 实现「继承」；由 `createMqttclient` 工厂创建具体实现。
- **线程与同步**：链表用 `pthread_mutex_t`；串口模块用 `pthread_mutex_t` + `sem_t`（异步收发线程）；MQTT 提供同步发送模式（`pushOneMessageWait`，基于 `sem_t`）。
- **Web 路由表驱动**：`mongoose/web_config.h` 以 `WEB_API_PATH` 路由树（`num/path/next_level/func_hand/type`）注册 HTTP 处理函数，统一 `web_code_rsp*` 封装响应。

## 四、项目模块

> 各模块的**详细接口 / 结构体 / 宏 / 错误码**、第三方库收纳明细与示例验证代码，统一见扩展文件 `PROJECT_模块明细.md`；本节只作模块清单与职责概览。

### 自研模块（清单与职责）

| 模块 | 目录 | 职责 |
|---|---|---|
| 字符串/编码 | `string/` | 十六进制与字符串/整数互转、大小端格式化、IP 字节-字符串转换、URL 提取 host、安全拷贝 |
| 文件/目录 | `file/` | 文件读写（含 fsync）、追加、删除、复制、移动、改名、大小/存在判断，目录创建/删除/存在判断 |
| 时间 | `timeopt/` | 标准时间/日期/日期时间获取与格式化、数组化拆分、秒/毫秒时间戳、系统时间与 RTC 设置、时间差计算 |
| JSON | `json/` | cJSON 第三方 + 字段提取与序列化/反序列化封装 |
| 日志 | `zlog/` | zlog 第三方 + 初始化/反初始化与分级日志宏封装 + 日志按占用/过期清理 |
| HTTP 客户端 | `curl/` | libcurl 封装：通用请求、超时请求、GET 文件、GET/PUT、上传（含二进制）、JSON 响应解析回调；另含云平台 ECP 业务接口 |
| MQTT 客户端 | `mqtt/` | 抽象 MQ 接口 + paho 实现：创建/销毁、订阅/退订、发布（异步/同步）、消息与断线回调 |
| Web 服务器 | `mongoose/` | mongoose 第三方 + 启停封装 + 路由表注册与统一响应封装 |
| 网络配置 | `netopt/` | 以太网 IP/掩码/网关/MAC/Link 读设、netlink 事件监听与网关列表、DHCP 启停与状态回调、DNS 读写、WiFi STA/AP 模式（基于裁剪 wpa_ctrl + udhcpd） |
| 串口 | `uart/` | 串口打开/配置/读（含超时）/写/关闭；异步封装（收发线程 + 锁 + 信号量 + 回调） |
| 链表 | `dataStruct/linkedList/` | 带互斥锁的双向链表，支持头插/尾插/按索引或节点删除/遍历/清空 |
| 杂项工具 | `misc/` | C 混合 shell 执行、崩溃转储到文件、MMC 容量/使用率统计（ioctl/statvfs）、MAC 与 SN 转换、CPU/内存使用率与温度、随机数 |

### 第三方模块（清单）

| 模块 | 目录 | 版本 |
|---|---|---|
| libcurl | `curl/include`、`curl/lib` | 7.85.0 |
| OpenSSL | `openssl/` | 1.1.1e |
| paho.mqtt.c | `mqtt/include`、`mqtt/lib` | 1.3.9 |
| libmodbus | `modbus/` | 3.1.11 |
| mongoose | `mongoose/` | 无版本文件 |
| zlog | `zlog/zlog-1.2.18`、`zlog/out_*` | 1.2.18 |
| zint | `zint/` | 2.13.0 |
| uthash | `include/uthash.h` | 无版本 |
| zlib | `zlib/` | 官网 zlib.net |
| libpng | `img/png/` | 1.6.45 |
| wpa_supplicant | `netopt/wpas/` | 2.10（裁剪） |

## 五、项目原则

> 本项目开发遵循的硬性约定（用户确立，逐条追加，不作删改）。

1. **模块功能独立、功能完备、接口不修改、只能添加**
   - 模块功能独立：各模块职责单一、自成一体，不与他模块交叉耦合。
   - 功能完备：模块自身能力闭环，不依赖其他模块补齐。
   - 接口不修改、只能添加：已对外提供的接口一经确定即不得修改，演进只能以**新增接口**的方式扩展（保持向后兼容）。

2. **通用能力沉淀基础库、业务极限瘦身**
   - 通用能力沉淀：业务工程中提炼出的通用代码优先沉淀回本库（base），持续积累通用能力。
   - 业务极限瘦身：业务工程只保留业务逻辑本身，尽量薄，避免在业务层重复造轮子。

3. **基础库不设业务性上限，由调用者斟酌**
   - 基础库只提供能力与状态（已写字节数、错误码等），不代替调用者决定业务阈值（容量上限、超时、重试次数等）。
   - 避免在库层硬编码业务判断；上限与策略由调用者按其场景自行决定。
