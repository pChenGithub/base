# 蒸馏记忆

- 蒸馏时间: 2026-11-05 20:07
- 依据文件: `PROFILE.md`、`pro_base/PROJECT.md`、`pro_base/PROJECT_模块明细.md`、`pro_base/logs/2026-09-18.md`、`pro_base/logs/2026-11-02.md`、`pro_base/logs/2026-11-03.md`
- 状态: 已同步

## 公共节

### 体系规则

- 记忆体系位于 `.codebuddy/memory/`，四部分单向依赖：素材（② `pro_<简称>/PROJECT.md`、③ `PROFILE.md`、④ `pro_<简称>/logs/`）→ 蒸馏 → ① `MEMORY.md`。
- `MEMORY.md` 只由蒸馏产生，**禁止直接编辑/追加**；新信息先写入素材。
- `MEMORY.md` 每次会话必加载（经桥接规则 `rules/memory_load.mdc`）；`PROFILE.md` 与 `PROJECT.md`、`logs/` 按需读取。
- 素材关键内容（定位/技术栈/架构/关键约定/协作要点）实质变化 → **实时蒸馏**；常规日志追加不单独触发蒸馏。
- **写入端义务（硬性，`rules/memory_load.mdc`「工作收尾的沉淀义务」）**：结构性 / 行为性变化（新增接口、改分层、改关键约定、修缺陷、引依赖）须追加当日日志（同一会话同类改动合并为一条）+ 涉及架构/模块/约定时同步更新 `PROJECT.md`，随后立即蒸馏；纯查询、格式微调、体系维护不写日志。
- **滞后检测（硬性，`rules/memory_load.mdc`「加载要求」第 3 条）**：会话开始比对 `MEMORY.md` 蒸馏时间 / `PROJECT.md` 修改时间 / `logs/` 最新日期，素材晚于蒸馏时间即判定**假同步**，先补蒸馏再继续任务。
- 日志 append-only、条目格式 `- [HH:MM] 主题：内容`、`[HH:MM]` 取真实本地时间；日志只记与具体项目相关的工作，体系维护类不记日志。
- 记忆文件的管理遵循 `skills/init-mem`；用户画像仅在用户明确要求时更新。
- 编码强制约束归 `.codebuddy/rules/*.mdc`，不写入 memory。

### 用户协作要点

- 中文沟通，简洁直接、结论先行；先给方案再给细节，必要时多方案对比；列举清楚、逻辑清晰、有理由据。
- 思考顺序：先整体后局部；先架构分析（涉及哪些层/文件/数据流向）再动手编码；自底向上、按分层顺序编码，禁止跳过中间层。
- 模块功能独立原则（架构与设计偏好）：模块/组件/skill 各司其职、独立自治，不接受职责交叉与相互引用；依赖必须单向。模块须**功能完备**（能力闭环，不依赖他模块补齐，不留半成品接口）；对外**接口不修改、只能新增**（演进靠新增接口保持向后兼容）。
- 通用代码沉淀基础库、业务极限瘦身：业务中出现的通用代码应沉淀回基础库、持续积累通用能力；业务代码只保留业务本身，尽量做极限瘦身（薄业务、厚基础）。
- 错误优先、提前退出：写函数先堵死所有失败路径，错误时立即记录原因并终止，正常逻辑零嵌套。
- 一次性解决，避免来回改：首次处理就把机制建好（约束固化进 rules/skill 自动生效）。
- 先澄清再执行：改动用户已有内容前，先说明问题 → 提出方案 → 获得确认；未经确认不改动既有内容。
- 规则态度：先执行，发现问题再改规则，不轻易变通；不喜欢已写明的规则被忽略、不喜欢同样问题反复提醒。
- 先堵失败路径再走正常流程；量力而行；检查验证无误才收尾，完成后复盘沉淀（体系化演进 + 版本化 + CHANGELOG）。
- 最不能容忍：反复返工、规则被无视。
- 代码质量要求：严格按 `.codebuddy/rules` 下 `code_c.mdc`、`code_cpp.mdc`、`code_general.mdc` 执行并自查（JSDoc API 注释、变量/结构体成员/宏注释、命名、下标从 0、错误分支必打日志、禁魔法数字等为硬性）；**禁止以「跟随存量代码风格」为由降低规范**；每次代码修改后立即做 x86 编译验证。
- 沟通常用语言为中文；用户为多面开发者（前端 Vue3/TS、后端 Java/Spring Boot/Node、嵌入式 C/Qt、运维 Docker/CI），工作领域为嵌入式 Linux / Qt C++ 工控设备（门禁 / 智能控制器）。

## 项目：base

### 定位

- 仓库 `base`（base lib and third lib）：基于 C 的基础库集合 = 自研公共接口 + 第三方 C 库封装/收纳。
- 形态：**qmake `.pri` 片段库**（`base.pri`），无顶层 `.pro`；宿主工程 `include(base.pri)` 并通过 `DEFINES += M_XXX` 按需裁剪编入模块。
- 第三方模块统一「三件套」管理：`source_code.txt`（上游来源）、`build.txt`（编译命令/工具链）、`.gitignore`（屏蔽源码与产物）。
- 注意：多个源文件 `#include "global.h"`，而仓库内不提供 `global.h`，须宿主工程提供。
- 素材扩展文件：`pro_base/PROJECT_模块明细.md`（承载各模块详细接口/结构体/宏/错误码、第三方库收纳明细、示例验证代码；四要素概览见 `PROJECT.md`）。**扩展文件主题后缀经用户确认为全中文风格**（统一 `PROJECT_<中文主题>.md`，本项目内不使用英文后缀，如后续创建即 `PROJECT_沟通纠正.md`）。

### 架构

- 「能力模块集合」：每个顶层目录为一个独立功能模块，自带 `.h/.c` 与 `*_errno.h` / `*Err.h`，由 `base.pri` 的 `contains (DEFINES, M_XXX)` 块条件编入。
- 条件编译映射：`M_MONGOOSE`(mongoose/webserver)、`M_CJSON`(json)、`M_MQTT`(mqtt+paho)、`M_MIXSHELL`/`M_DEBUG`/`M_MMC`(misc)、`M_STRING`、`M_FILE`、`M_TIME`、`M_HTTPC`(curl+openssl)、`M_LOG`(zlog)、`M_NET`(netopt)、`M_NETMGR`(netmgr)。
- 统一错误码：各模块头文件定义 `XXXERR_*`，接口返回负错误码。
- 回调驱动：netlink 事件、DHCP 状态、串口收发、MQTT 消息、HTTP 响应、shell 逐行输出均为函数指针回调。
- C 语言面向对象：`mqttClient.h` 抽象接口 `MQClient` + `MQTYPE`，`mqttPahocClient` 以首成员 `MQClient base` 实现「继承」，工厂 `createMqttclient` 创建实现。
- 同步原语：链表 `pthread_mutex_t`；串口 `pthread_mutex_t` + `sem_t`（收发线程）；MQTT 同步发送 `pushOneMessageWait`（`sem_t`）。
- Web 路由表驱动：`mongoose/web_config.h` 以 `WEB_API_PATH` 路由树注册处理函数，`web_code_rsp*` 统一响应。
- netmgr 编排层：`netmgr/` 管理以太网/WiFi/4G 多链路的上网主备，**不复用 netopt 接口**（仅参考其实现机制）；四层目录 `api → core → sys → common`（2026-11-03 撤销 linktype，三条链路实现并入 core）；优先级即默认路由 metric，内核按 metric 选路并自动回退；**core 不建线程**，netlink 事件由 sys 回调直接驱动一轮仲裁，其余状态变化由业务 `netmgr_poll` 补齐。

### 模块与关键约定

- **项目原则（硬性）**：① 模块功能独立、功能完备，已对外接口不修改、只能新增（演进靠新增接口保持向后兼容）；② 通用能力沉淀基础库、业务极限瘦身（通用代码沉淀回 base，业务工程只留业务逻辑）；③ 基础库不设业务性上限，由调用者斟酌（只提供能力与状态，不代替调用者决定容量/超时/重试等业务阈值）。完整条目见 `pro_base/PROJECT.md` 第五节。
- 自研模块：`string/`(编码转换/安全拷贝/URL 取 host)、`file/`(文件与目录操作)、`timeopt/`(时间与时间戳)、`json/`(cJSON 封装)、`zlog/`(日志封装与清理)、`curl/`(HTTP 客户端 + ECP 云平台业务)、`mqtt/`(抽象 MQ 接口 + paho 实现)、`mongoose/`(Web 服务器封装)、`netopt/`(IP/网关/MAC/DHCP/netlink/DNS/WiFi)、`uart/`(串口同步与异步)、`dataStruct/linkedList/`(加锁双向链表)、`misc/`(shell/崩溃转储/MMC 容量/CPU 内存/随机数)、`netmgr/`(以太网/WiFi STA+AP/4G ECM 链路管理、四态状态机与运行时开关、优先级主备、14 类统一事件、WiFi 扫描，四层自包含)。
- netmgr 分层与边界（硬性）：netopt 只作实现参考，netmgr 不 include netopt 头、不调其函数；链路按 `NETMGR_OPS`(open/close/refresh) 扩展接入方式（PPP/QMI 已留位），操作表为 core 内部契约、不外露；路由必须走 rtnetlink 带 `RTA_PRIORITY`（ioctl `SIOCADDRT` 无法表达 metric）；AP 模式 `gate` 恒空、不参与默认路由。
- netmgr 关键约定（硬性）：① 链路四态 `DISABLED/DOWN/READY/ACTIVE`，`link_enable(type,on)` 运行时开关按缓存配置重建；② 唯一回调 `netmgr_event_cb`，`NETMGR_EVENT` 外层只有 `ev`、其余为联合体裁荷，14 类事件；③ 事件在锁外分发，回调内可安全调本模块接口；④ **模块内部禁止打印日志**（`code_*.mdc` 的"错误分支必打日志"在本模块不适用），异常以 `NETMGRERR_*` 返回、原因随 `NETMGR_LINK.err` 上报；⑤ WiFi 控制协议在 `sys/net_mgr_wpa`（core 不碰 socket 协议）；⑥ 类型随产出模块：`NETMGR_ROUTE`→`route.h`、WiFi 结果→`wpa.h`、对外类型→`core.h`，`common` 只留长度常量；⑦ wpa 网络配置不持久化：每次连接重建、失败按编号回滚、断开即清空，防自动重连；国家码经 `netmgr_wpa_conf_write` 入参下发，不传则不写 `country` 行；wpa 事件通道用双 socket（命令/监听分离）+ sys 层接收线程（core 仍不建线程），通道未打开统一返回 `NETMGRERR_STATE`；wpa 自管 wpa_supplicant 进程（`netmgr_wpa_start/stop`：kill 残留 → 写配置 → 拉起 → 等接口 → 开通道；停止时先断开再关通道再 kill），进程控制能力下沉到 `common/net_mgr_proc`，sys 层仍零横向依赖；hostapd/udhcpd 等跨进程互斥仍归 core；⑧ 链路操作表 `NETMGR_OPS` = open/close/refresh：refresh 只投影状态（只写 ctx 的 ip/mask/gate/state/err），不含启停动作；DHCP 为 **sys 层长驻会话**（`udhcpc -f`，无 `-n`/`-q`/`-T`：失败由 udhcpc 自身持续重试、成功自身续租；回调 `netmgr_dhcp_cb` 无返回值，SUCC 只报一次，FAIL 在“会话内超 10 秒未取到租约”或“进程异常退出且未取到租约”时上报，后者本层节流重建），可靠性不下放到仲裁层轮询；⑨ netlink 事件类别按**真实标志**判定：RTM_NEWLINK 由 `IFF_UP|IFF_RUNNING` 拆为 `LINK_UP`/`LINK_DOWN`，`RTM_DELLINK` 才是 `LINK_DEL`（插线与拔线内核都发 RTM_NEWLINK，不能只看消息类型）。 ⑩ 仲裁机制独立在 `core/net_mgr_arbiter`（五阶段：刷新→差异→路由→选举→应用），只产出事件数组；`core_tick` 负责加锁调用与锁外分发，仲裁内不启停进程。
- 第三方收纳：`modbus/`(3.1.11)、`zint/`(2.13.0 条码二维码)、libcurl 7.85.0、OpenSSL 1.1.1e、paho.mqtt.c 1.3.9、zlog 1.2.18、mongoose、uthash、zlib、libpng 1.6.45、wpa_supplicant 2.10（裁剪 `wpas/`）。
- 编译目标：aarch64（gcc-linaro 10.2.1）、arm-linux-gnueabihf（devkit/A8）、x86。
- 现状约定（未改动，仅记录）：`base.pri` 未纳入 `dataStruct/linkedList`、`modbus`、`uart`、`zint`、`img`、`include/uthash.h`，以及 `misc/mac_opt.c`、`misc/rand_ops.c`、`netopt/net_dns.c`、`netopt/wifi_opt.c`、`netopt/wifi_opt_slf.c`、`curl/zyEcpHttpReq.c`；`M_MMC` 块上方注释误写为「net opt」；`netopt/example/Makefile` 引用多个不存在的测试文件，`netopt/example/pro_wifi` 为未纳入版本控制的编译产物；MQTT 的 C++ `taskMqttBase.cpp` 已在 `base.pri` 中注释停用。
- 验证入口：`netopt/example/`（`Makefile` 交叉编译 `pro_wifi`，`test_wifi.c`/`test_gate.c` 为演示程序）；`netmgr/example/`（`net_demo.c` 静态配置初始化 + 9 个目标：`net_demo`、仲裁层交互示例 `demo_core`（eth+wifi 双链路，覆盖 core 全部接口）与 7 个 sys 模块独立示例 `demo_iface`/`demo_route`/`demo_dhcp`/`demo_proc`/`demo_dns`/`demo_event`/`demo_wpa`，各只链接对应模块源文件）。
