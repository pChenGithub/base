# base 项目模块明细

- 更新日期: 2026-11-02
- 状态: 已初始化（自 `PROJECT.md` 拆分）；2026-11-02 补入 `netmgr/` 明细；2026-11-03 按四层重构更新 `netmgr/` 明细
- 说明: 本文件为 `pro_base/PROJECT.md` 的扩展文件，承载各模块详细接口/结构体/宏/错误码、第三方库收纳明细与示例验证代码；项目四要素概览见同目录 `PROJECT.md`。

## 一、自研模块明细

### 1. string/ — 字符串与编码转换

- **编译开关**：`M_STRING`｜**源文件**：`string_opt.c` / `string_opt.h`；构建片段 `string/string.pri`
- **职责**：十六进制与字符串/整数互转、大小端格式化、IP 字节-字符串转换、URL 提取 host、安全拷贝、十六进制日志打印。
- **接口**：
  - 编码转换：`hexStr2Array`、`hexArray2Str`、`hexStrStriptail`、`hexArray2Int`、`hex2Int`、`hexStr2ll`、`hexStrLittle2ll`、`hexstrLittle2Decstr`
  - 安全拷贝：`strCopyC`、`strCopyCArray`（宏 `STR_COPY`）
  - 数值/字符串格式化：`ll2Str`、`i2Str`、`C2Str`、`litleFormatByte`、`litleFormatShort`
  - IP / URL：`ipByte2Str`、`ipStr2Byte4`、`getHostFromUrl`
  - 调试输出：`printLog`
- **错误码**：`STROPTERR_CHECKPARAM`

### 2. file/ — 文件与目录操作

- **编译开关**：`M_FILE`｜**源文件**：`file_opt.c` / `file_opt.h`；构建片段 `file/file.pri`
- **职责**：文件读写（含落盘 fsync）、追加、删除、复制、移动、改名、大小与存在判断；目录创建/删除/存在判断。
- **接口**：`file_read`、`file_write`、`file_write_fsync`、`file_append`、`file_remove`、`file_copy`、`file_move`、`file_rename`、`file_size`、`file_exist`、`dir_create`、`dir_remove`、`dir_exist`
- **错误码**：`ERR_FILEOPT_*`

### 3. timeopt/ — 时间操作

- **编译开关**：`M_TIME`｜**源文件**：`timeopt.c` / `timeopt.h`
- **职责**：标准时间/日期/日期时间获取与格式化、数组化拆分、秒/毫秒时间戳、系统时间与 RTC 设置、时间差计算。
- **接口**：`getStdTime`、`getStdDate`、`getStdDateTime`、`getArrayDate`、`getArrayTime`、`getSTime`、`getMsTime`、`getStdDateTimeStr`、`setSysTime`、`setSysTimeByRTC`、`getTimeDiff`
- **错误码**：`TIMEOPT_ERR_*`

### 4. json/ — JSON（cJSON）及封装

- **编译开关**：`M_CJSON`｜**源文件**：`cJSON.c`、`cJSON_tools.c` / `cJSON.h`、`cJSON_tools.h`、`json_errno.h`；构建片段 `json/json.pri`
- **职责**：内置完整 cJSON（第三方），并封装字段提取与序列化/反序列化。
- **接口**：
  - 字段提取：`getfield_strPoint`、`getfield_str`、`getfield_int`、`getfield_float`、`getfield_double`、`getField_arrayInt`、`getField_arrayInt2`、`getField_obj`
  - 序列化：`json2string`、`json2string_free`、`string2json`、`string2jsonArray`
- **错误码**：`ERR_JSON_*`（`json_errno.h`）

### 5. zlog/ — 日志（zlog）及封装与清理

- **编译开关**：`M_LOG`｜**源文件**：`lib_log.c`、`lib_logTool.c` / `lib_log.h`、`lib_logTool.h`、`lib_logErr.h`
- **依赖**：第三方 zlog 1.2.18（`-Lzlog/lib -lzlog`）
- **职责**：日志初始化/反初始化、分级日志宏、日志文件按占用与过期策略清理。
- **接口**：
  - 日志：`lib_log_init`（内部 `dzlog_init(conf, "zyck")`）、`lib_log_deinit`（`zlog_fini`）、宏 `LOG_I` / `LOG_D` / `LOG_E` / `LOG_TEST`、全局 `lib_log_debug_flag`
  - 清理：`delBigLogs`、`delBigFile`、`delOverdueLogs`（内部 `delPre5Logs`，基于 `scandir` 过滤目录）
- **错误码**：`LIBLOG_ERR_*`

### 6. curl/ — HTTP 客户端（libcurl 封装）

- **编译开关**：`M_HTTPC`｜**编入源文件**：`ifaceHttpReq.c` / `ifaceHttpReq.h`（`zyEcpHttpReq.c` 未纳入 `base.pri`）
- **依赖**：libcurl 7.85.0（`-Lcurl/lib -lcurl`）+ OpenSSL（`-Lopenssl/buid_aarch64/lib -lssl -lcrypto`）
- **职责**：通用/超时 HTTP 请求、GET 文件、GET/PUT、上传（含二进制）、JSON 响应解析回调；另含云平台 ECP 业务接口（未纳入 `base.pri`）。
- **关键类型**：`HTTP_REPLY`（type/opt/p/len/offset/ret）、`HTTP_REPLY_TYPE`（`RET_NONE` / `RET_FILE` / `RET_JSON`）、回调 `HTTP_REPLY_CONTENT_FUNC`
- **接口**：
  - 通用：`httpReq`、`httpReqTimeout`、`httpGetReq`、`httpPutReq`、`httpGetFileReq`、`httpUploadFile`、`httpPutUploadBinaryFile`
  - ECP 云平台：`httpGetToken`、`httpAddEcpDev`、`httpDelEcpDev`、`httpGetproperty`、`httpGetpropertyBysn`、`httpSetproperty`、`httpSetCmd`、`httpBroadcastCmd`、`httpGetScenemodeBysn`
- **错误码**：`HTTPERROR_*`（含 `HTTPERROR_PARSE_JSON`、`HTTPERROR_GET_FILED_VAL`、`HTTPERROR_GET_TOKEN`）

### 7. mqtt/ — MQTT 客户端（抽象接口 + paho 实现）

- **编译开关**：`M_MQTT`｜**编入源文件**：`mqttPahocClient.c` / `mqttClient.h`、`mqttPahocClient.h`（C++ `taskMqttBase.cpp` / `taskMqttBase.h` / `taskMqttMeta.h` 已在 `base.pri` 中注释停用）
- **依赖**：paho.mqtt.c 1.3.9（`-Lmqtt/lib -lpaho-mqtt3c`）
- **设计**：抽象接口 `MQClient`（含 `MQTYPE type`）+ 类型枚举 `MQTYPE`（`MQTYPE_PAHOC`）；paho 实现 `MQClientPahoC` 以首成员 `MQClient base` 实现「继承」；工厂 `createMqttclient` 创建实现。
- **接口**：`createMqttclient`、`destroyMqttclient`、`destroyMqttclient2NULL`、`setCallBack`、`pushOneMessage`（异步）、`pushOneMessageWait`（同步等回应）、`subscribeTopic`、`unSubscribeTopic`
- **回调**：`handleMessage`、`handleLost`、`handleComplate`
- **关键宏**：`MQSYNC`、`QOS=1`、`LOCKTIMEOUT_10MS`、`SENDMSG_TIMEOUT_MS`、默认 `MQ_USERNAME` / `MQ_PASSWORD`；保留标志 `RETAINED_TYPE`
- **错误码**：`MQERR_*`；另 `mqttServer.h` 为空壳（仅 include guard）

### 8. mongoose/ — Web 服务器封装

- **编译开关**：`M_MONGOOSE`｜**源文件**：`mongoose.c`（第三方）、`webserver.c` / `webserver.h`、`web_config.h`
- **职责**：内嵌 mongoose 网络/Web 服务器 + 启停封装 + 路由表注册 + 统一响应封装。
- **接口**：`webAppStart`、`webAppStop`、`WEB_USER_CONF`；响应封装 `web_code_rsp`、`web_code_rsp_str`、`web_code_rsp_int`；`HTTP_SEND_OK`；`WEB_CODE_OK` / `WEB_CODE_ERR`
- **路由模型**：`WEB_API_PATH`（`num` / `path` / `next_level` / `func_hand` / `type`）、`HTTP_REQ_TYPE`（`TYPE_REQ_POST` / `POST_FILE` / `LINK_FILE` / `POST_CODE`）、`HAND_HTTP_REQ`、`LINK_FILE_ARG`
- **错误码**：`WEBAPP_ERR_*`

### 9. netopt/ — 网络配置（以太网 / netlink / DHCP / DNS / WiFi）

- **编译开关**：`M_NET`｜**编入源文件**：`net_opt.c`、`net_dhcp.c`、`net_netlink.c` / `net_opt.h`、`net_dhcp.h`、`net_netlink.h`
- **未纳入 `base.pri` 的源文件**：`net_dns.c` / `net_dns.h`、`wifi_opt.c` / `wifi_opt.h`、`wifi_opt_slf.c`
- **公共定义**：`net_errno.h`（`NETERR_*`，含 WiFi 段 100 起）、`net_def.h`（`IP_STR_MAXLEN`、`NET_LINK_UP` / `NET_LINK_DOWN`）
- **职责与接口**：
  - 以太网参数：`get_ip` / `set_ip`、`get_mask` / `set_mask`、`get_mac` / `set_mac`、`get_link`、`set_enable`、`isIpValid`
  - 网关：`set_gateway`、`set_gateway_shell`、`replace_gateway`、`del_gateway`
  - netlink 监听：`start_netlink_listen` / `stop_netlink_listen`、`set_netlink_listen_ev` / `rm_netlink_listen_ev`、`nl_getGateways`；类型 `NETLINK_CB_PARAM`、`NLEV_TYPE`（`NLEV_LINK`）、`ROUTE_LIST`；回调 `hand_netlink_ev`
  - DHCP：`start_dhcp` / `stop_dhcp`、`DHCP_STATUS`、回调 `hand_dhcpstatus`、`NET_DHCP_AGAIN`
  - DNS：`set_resolv_dns` / `get_resolv_dns`、`DNS_INFO`
  - WiFi（STA）：`wifi_sta_enable` / `disable` / `scan` / `scan_result` / `connect`
  - WiFi（AP）：`wifi_ap_enable` / `reload` / `disable`；配置类型 `WIFI_SECURITY`、`AP_CONFIG`、`UDHCPD_CONFIG`、`WIFI_AP_CONFIG`
- **wpa_supplicant 集成**：`wpas/` 为 2.10 裁剪版（`wpa_ctrl.c/.h`、`os_unix.c`、`os.h`、`common.h`、`wpabuf.h`、`wpa_debug.h`、`build_config.h`、`includes.h`）；集成需定义 `CONFIG_CTRL_IFACE`、`CONFIG_CTRL_IFACE_UNIX`，并需为 `wpa_ctrl.h` 补 `#include <stdlib.h>`；`wpa_supplicant.conf` 配置 `ctrl_interface=/var/run/wpa_supplicant`、`ap_scan=1`

### 10. uart/ — 串口操作

- **编译开关**：未纳入 `base.pri`｜**源文件**：`uart.c` / `uart.h`、`uart_moudle.c` / `uart_moudle.h`
- **职责**：串口打开/配置/读（含超时）/写/关闭；异步封装（收发线程 + 互斥锁 + 信号量 + 回调）。
- **接口**：
  - 基础：`uart_open`、`uart_setopt`、`uart_read`、`uart_read_timeout`、`uart_write`、`uart_close`
  - 封装：`start_uart`、`stop_uart`、`set_uart`、`set_uart_ck`、`send_byte_uart_wait`、`send_uart_wait`、`send_uart_one`
- **关键类型**：`BAUDRATE_TYPE`、`STOPBIT_TYPE`、`EVENT_TYPE`；`UART_ENTRY`（fd / 收发线程 / 收发缓冲 / 锁 / 信号量 / 回调）；回调 `hand_uart_msg`
- **错误码**：`UARTOPTERR_*`、`UART_MOUDLE_ERR_*`

### 11. dataStruct/linkedList/ — 加锁双向链表

- **编译开关**：未纳入 `base.pri`｜**源文件**：`linkedList.c` / `linkedList.h`
- **职责**：带互斥锁的双向链表，支持头插/尾插/按索引或节点删除/遍历/清空。
- **关键类型**：`LINK_NODE`（pre / next / head）、`LINK_HEAD`（node / nodecount / lock_linked，含 `pthread_mutex_t`）
- **接口**：`init_linkedlist`、`free_linkedlist`、`insert_lknode`、`insert_head`、`insert_tail`、`foreach_lklist`、`remove_lknode_index`、`remove_lknode`、`clear_lklist`
- **宏**：`INIT_LINKEDLIST`、`FOREACH_LKLIST`、`CLEAR_LKLIST`、`OFFSETOF`、`CONTAINER_OF`
- **错误码**：`LKLIST_ERR_*`

### 12. misc/ — 杂项工具

- **mixShell（`M_MIXSHELL`）**：`mixShell.c` / `mixShell.h`、`mixShell_errno.h`；C 混合 shell 执行——`exeShell`（执行取首行）、`exeShellWait`（回调逐行处理）；类型 `SHELL_RET_TYPE`、回调 `shell_ret_line`；错误码 `MIXSHELLERR_*`
- **debug（`M_DEBUG`）**：`debug_opt.c` / `debug_opt.h`；接口 `debug_get_crash_2_file`（崩溃信息转储到文件）
- **MMC（`M_MMC`）**：`mmc_opt.c` / `mmc_opt.h`；接口 `mmc_getsize`、`mmc_getUseSize`、`mmc_getFreePercent`、`mmc_getUsagerate`（基于 ioctl / statvfs）
- **mac（未纳入 `base.pri`）**：`mac_opt.c` / `mac_opt.h`；接口 `mac_to_snstr`、`mac_to_snarray`、`get_cpu_temperature_raw`、`get_cpu_temperature`、`get_cpu_Usagerate`、`get_mem_Usagerate`
- **rand（未纳入 `base.pri`）**：`rand_ops.c` / `rand_ops.h`；接口 `rand_get`、`random_get`、`rand_get_0to3600`

### 13. netmgr/ — 网络管理服务

- **编译开关**：`M_NETMGR`｜三层目录共 16 个源文件：`net/net_m_core.c` + `net_m_arb.c` + `net_m_route.c` + `net_m_link.c` + `net_m_eth.c` + `net_m_wifi.c` + `net_m_cell.c`；`sys/net_mgr_iface.c` + `net_mgr_route.c` + `net_mgr_dhcp.c` + `net_mgr_dns.c` + `net_mgr_wpa.c` + `net_mgr_ap.c` + `net_mgr_event.c`；`common/net_mgr_util.c` + `net_mgr_proc.c`｜构建片段 `netmgr/netmgr.pri`（默认 `all`；`NETMGR_LEVEL=sys` 仅编入 common+sys）
- **依赖**：`-lpthread`；外部命令 `udhcpc`、`wpa_supplicant`、`hostapd`、`udhcpd`。**不依赖 netopt 及 base 内其它自研模块**
- **职责**：以太网 / WiFi / 4G 三类链路统一管理；四态状态机与运行时开关；按优先级（= 默认路由 metric）实现主备自动切换；DNS 下发；16 类统一事件；WiFi 扫描
- **分层（依赖单向 `net → sys → common`；2026-11-09 迁移完成，旧 `api/`、`core/` 已删除）**：
  - `net/`：仲裁编排层，单线程消息循环 + 唯一所有者线程；`net_m_core.c` 负责生命周期、命令队列、自愈与事件分发，`net_m_arb.c` 为五阶段仲裁器（刷新 → 差异识别 → 路由同步 → 主链路选举 → DNS/切换应用），`net_m_route.c` 做默认路由同步（先建新再删旧），`net_m_link.c` 为就绪判定公共实现，`net_m_eth/wifi/cell.c` 为三条链路实现，操作表 `NETMGR_OPS` 属本层内部契约（`net_m_ctx.h`），对外只暴露 `net_m_def.h`
  - `sys/`：`iface`（ioctl 接口）、`route`（rtnetlink 默认路由）、`dhcp`（udhcpc 取址）、`dns`（resolv.conf）、`wpa`（wpa_supplicant 控制协议、事件与**进程生命周期**——`netmgr_wpa_start/stop` 自管拉起与结束）、`ap`（AP 侧 hostapd + udhcpd 的配置生成与**进程生命周期**——`netmgr_ap_start/stop/running`，不碰 iface）、`event`（netlink 事件）；七模块零横向依赖
  - `common/`：错误码、通用校验、**进程控制 `net_mgr_proc`**（`/proc` + `system`，供 sys 层模块自管外部进程）；原 `net_mgr_limits.h` 已删除，接口名 / IPv4 字符串按规范直接写数组长度 16
- **对外接口（`net_m_def.h`，`netm_*`）**：
  - 生命周期：`netm_start`（总配置 + 统一事件回调 + user；配置内部复制）、`netm_stop`（撤路由、关链路、回收管理线程）
  - 查询（只读快照，任意线程可高频调用）：`netm_link_get`、`netm_link_list`（容量入参 / 条数出参）、`netm_active_get`、`netm_prio_get`
  - 开关：`netm_link_enable(type, on)`（按缓存配置重建，立即生效）
  - 优先级：`netm_prio_set`、`netm_prio_check`（类型级，0-255，小者优先；冲突返回 `-NETMGRERR_PRIO`）
  - 链路配置：`netm_eth_set`、`netm_wifi_set`、`netm_cell_set`（先撤路由 → 关链路 → 按新配置拉起 → 立即仲裁）
  - DNS：`netm_dns_set`（写 `/etc/resolv.conf`，空串表示清除）
  - WiFi 扫描：`netm_wifi_scan(list, num)`（同步触发并等结束，结果直接写入调用方数组，事件 `apNum` 与写入条数同源一致）、`netm_wifi_scan_result`（读取最近一次扫描结果）、`netm_wifi_status`（当前连接信息）
- **关键类型**：
  - `NET_M_TYPE`（`NET_M_ETH` / `NET_M_WIFI` / `NET_M_CELL`，取值同时作内部数组下标）
  - `NET_M_STATE`（`NET_M_ST_DISABLED` / `DOWN` / `READY` / `ACTIVE`；`DISABLED` 为用户显式关闭，`ACTIVE` 仅出现在快照中、由仲裁叠加）
  - `NET_M_EV` 16 类：`UP` / `DOWN` / `SWITCH` / `ENABLED` / `DISABLED` / `INFO_CHANGE` / `ROUTE_CHANGE` / `DHCP_RUNNING` / `DHCP_SUCC` / `DHCP_FAIL` / `DHCP_LOST` / `WIFI_CONNECTED` / `WIFI_DISCONNECTED` / `WIFI_SCAN_DONE` / `FAULT` / `DEGRADED`（后两类为 net 层新增的故障可观测性）
  - `NET_M_EVENT`：外层只有判别符 `ev`，其余为联合体 `u`（`link` / `dhcp` / `wifi` / `info` / `sw` / `route` / `scan` / `fault`）；回调 `net_m_event_cb`，在管理线程上下文执行，回调内可安全再调本模块接口（识别自调用、内联执行）
  - `NET_M_LINK`（快照，含 `ip/mask/gate/prio/err/fault/faultStage/wifi`）、`NET_M_LINKREF`、`NET_M_WIFI_AP` / `NET_M_WIFI_INFO`（typedef 自 `sys/net_mgr_wpa.h`）
  - 配置：`NET_M_ETH_CONF`、`NET_M_WIFI_CONF`（STA 用 `cand[4]` 候选热点列表 + `candNum`，prio=0 按信号强度择优；AP 含 `apSsid/apIp/apMask/apStart/apEnd`）、`NET_M_CELL_CONF`、`NET_M_CONF`（含 `tickMs`）
  - 内部：`net_m_ctx.h` 的链路槽与操作表；链路实现只写 `priv/ifname/ip/mask/gate/dns/state/err`，`prio/route_on/rmetric/fault/prev` 归仲裁与路由同步独占；结构体内不含锁（仅管理线程写）
- **实现要点**：
  - 路由：`RTM_NEWROUTE` / `RTM_DELROUTE` + `RTA_GATEWAY` / `RTA_OIF` / `RTA_PRIORITY`；ioctl `SIOCADDRT` 无法可靠表达 metric，故不采用；net 层同步改为**先建新再删旧**，失败保留 `route_on` 便于重试
  - 路由查询：`RTM_GETROUTE` dump，筛 `dst_len==0` 的 IPv4 默认路由并保留 metric 最小者（`netmgr_route_act` 全表 / `netmgr_route_dev` 指定接口）
  - 事件：socket 订阅 `RTMGRP_LINK|RTMGRP_IPV4_IFADDR|RTMGRP_IPV4_ROUTE`（可传 `groups` 按需扩展）；事件细分为 6 类并带 `family/flags/metric`；识别 `NLMSG_OVERRUN` 上报 `NETMGR_NEV_LOST`；`SO_RCVBUF` 128KB
  - 仲裁驱动：`netm_start` 自建 **1 个管理线程**，周期定时（`NET_M_CONF.tickMs`，<=0 取默认 500ms）、netlink 事件、DHCP 回调、业务配置全部作为消息进入同一线程串行处理；就绪链路各下发 `default metric=prio`，`prio` 最小者为 `ACTIVE`；刷新失败不再静默跳过（降级 DOWN、撤路由、上报 `NET_M_EV_FAULT`），DNS 每轮同步（内部去重）
  - WiFi STA：进程生命周期由 `sys/net_mgr_wpa` 自管（`netmgr_wpa_start`：kill 残留 → conf_write → spawn → wait → open；`netmgr_wpa_stop` 反向；net 层只调用）；控制协议为 **unix dgram 裸命令/裸回包**（`CTRL-REQ` 包裹不被使用，回包失败以 `FAIL` 起头），**客户端 socket 必须 bind 本地地址**否则服务端回包无处投递（命令全部超时），实现用 `/tmp/netmgr_wpa.<pid>.{cmd,ev,probe}.sock` 并在 close 时删除；等待一律按挂钟计时（wait 10s、scan 8s、connect 10s），控制口就绪用“临时 socket + PING 应答”三态探测（有应答/连不上=残留/连上不应答=忙），start 前清理残留 socket，`REMOVE_NETWORK all` → `ADD_NETWORK` → `SET_NETWORK ssid/psk` → `ENABLE_NETWORK` → `SELECT_NETWORK` → 等 `wpa_state=COMPLETED` → DHCP 或静态地址；配置**不持久化**（`wpa.conf` 不写 `update_config`，每次连接重建网络；国家码经 `netmgr_wpa_conf_write` 入参下发，NULL 不写 `country` 行），连接任一步失败按编号 `REMOVE_NETWORK` 回滚，`netmgr_wpa_disconnect` 断开后清空全部网络避免自动重连；`NETMGR_WIFI_INFO.signal` 由 `SIGNAL_POLL` 的 `RSSI` 补齐，`SCAN_RESULTS` 用 8KB 堆缓冲接收、超长返回 `-NETMGRERR_BUFF` 不截断，加密判定匹配 `WPA`/`RSN`/`WEP`；通道未打开统一返回 `-NETMGRERR_STATE`，`netmgr_wpa_is_open()` 可查询；事件通道双 socket（命令/监听分离 + `ATTACH`）+ sys 层接收线程，上报 `CONNECTED/DISCONNECTED/SCAN_DONE/ASSOC_REJECT/TERMINATING` 五类（`netmgr_wpa_event_open/close`，close 与 `netmgr_wpa_close` 联动）；扫描默认等待 8s，超时后扫描仍在后台、结果可读
  - WiFi AP：配置与进程在 `sys/net_mgr_ap`（hostapd `/tmp/netmgr_ap.conf`、udhcpd `/tmp/netmgr_dhcpd.conf`，`dns` 非空才写 `opt dns`；`netmgr_ap_start` 先停旧实例再按新配置拉起，`netmgr_ap_running` 以 hostapd 存活为判据）；net 层负责**先配置接口地址再启动服务**、STA/AP 互斥（同一射频不并发）与 `gate` 恒空不参与默认路由
  - DHCP（**长驻会话模型**）：`popen` 执行 `udhcpc -i X -p pidfile -f`（**不加 `-n`/`-q`**：取不到租约由 udhcpc 自身持续重试、取到后常驻自身续租，会话只在 stop 或进程异常退出时结束，修复了原“一次性取址、租约到期无人续租”的缺陷）+ 逐行判定输出；状态回调 `RUNNING / SUCC / FAIL(err)`，**SUCC 仅在会话首次拿到租约时上报**（续租不重复），FAIL 有两种来源：会话内超过 10 秒未取到租约（持续重试，成功后再报 SUCC）、udhcpc 异常退出且未取到租约（本层节流 1 秒自动重建）；已取得租约后又退回发现阶段（`sending discover`）上报一次 `LOST`，重新取到再报 SUCC；会话内解析租约行得到地址与租约时长，`netmgr_dhcp_lease_get(ifname, NETMGR_DHCP_LEASE*)` 可查（`remainSec` 按获取时刻 + 租期 - 现在推算，过期 valid=0；服务器标识不解析）；`netmgr_dhcp_running` 表示会话运行中（含续租阶段）；会话由链路 open 建立、close 停止（按 pidfile 发 SIGTERM）；**stop 支持“自线程”调用**——业务在 DHCP 回调里关链路时，stop 会在取址线程内被调用，实现里用 `pthread_equal(pthread_self(), item->tid)` 检测后只置 `stopReq` + 结束 udhcpc 并立即返回 0（不等待自己收尾），否则会白等 3 秒并返回假错误 `-NETMGRERR_TIMEOUT`；同理 `netmgr_wpa_event_close` 在事件线程内被调用时跳过 `pthread_join`（否则自 join 死锁）。这两处检测属关键防御，不可当冗余删除；本层不打日志、失败返回负错误码
  - 就绪判定：`net_m_link` 统一实现「接口状态 + 地址 + 网关」组合判定（三条链路共享，避免口径漂移）
  - 日志：模块内部**不打印任何日志**，异常以 `NETMGRERR_*` 负值返回
- **错误码**：`NETMGRERR_*`（200~216，避开 netopt 0~110；**全模块单一来源** `common/net_mgr_err.h`，net 层不再自建错误码头文件；215 `PRIO`、216 `NOTSUP` 为 net 层新增）
- **示例**：`netmgr/example/`，`demo_net.c`（net 层交互式命令示例：启动即拉起服务，命令覆盖 netm_* 全部 16 个接口——链路快照/单链路/主链路、优先级查询-设置-预检查、链路开关、eth/wifi(STA,AP)/cell 重配、DNS 下发、WiFi 扫描/结果/连接信息、start/stop；空闲每 5 秒检测链路变化）+ 8 个 sys 模块独立示例（`demo_iface` / `demo_route` / `demo_dhcp` / `demo_proc` / `demo_dns` / `demo_event` / `demo_wpa` / `demo_ap`），各自只链接对应模块源文件
- **已知限制**：4G 仅 ECM/RNDIS/NCM（PPP、QMI 返回 `NETMGRERR_NOTSUP`）；AP 信道固定 6、依赖 hostapd / udhcpd；**STA 与 AP 按互斥设计**（同一射频不并发，不做能力探测，AP+STA 共存需另立双 vif + NAT 需求）；未在目标板实机验证真实 WiFi/4G

## 二、第三方库收纳明细

| 库 | 目录 | 版本 | 仓库内形态与备注 |
|---|---|---|---|
| libcurl | `curl/include`、`curl/lib` | 7.85.0 | 完整头文件 + 预编译 `libcurl.so.4.8.0`；`build.txt` 记录 aarch64 交叉编译（禁用 mqtt/ftp/rtsp 等） |
| OpenSSL | `openssl/` | 1.1.1e | 仅 `build.txt`（`./config no-asm no-async -shared`，aarch64）；产物 `buid_aarch64/` 被 gitignore |
| paho.mqtt.c | `mqtt/include`、`mqtt/lib` | 1.3.9 | 完整头文件 + 预编译（`-lpaho-mqtt3c`）；`build.txt` 记录 `cmake ../paho.mqtt.c-1.3.9`；另含 mosquitto 2.0.15 构建记录 |
| libmodbus | `modbus/` | 3.1.11 | 全量源码（`modbus.c`、`modbus-rtu.c`、`modbus-tcp.c`、`modbus-data.c` 及私有头）；RTU/TCP 主站读写位/寄存器（`modbus_read_bits`、`modbus_read_registers`、`modbus_write_bit`、`modbus_write_registers`、`modbus_write_and_read_registers` 等）；类型 `modbus_t`、`modbus_mapping_t` |
| mongoose | `mongoose/` | 无版本文件 | 源码内联（`mongoose.c` / `mongoose.h`） |
| zlog | `zlog/zlog-1.2.18`、`zlog/out_x86`、`zlog/out_devkit` | 1.2.18 | 源码 + 预编译产物（`libzlog.so.1.2.12`、`libzlog.a`、`include/zlog.h`、`bin/zlog-chk-conf`），含 `build_x86/`、`build_devkit/` |
| zint | `zint/` | 2.13.0 | 全量源码：码制模块（`qr.c`、`code128.c`、`pdf417.c`、`dmatrix.c`、`aztec.c`、`gs1.c`、`rss.c`、`upcean.c`、`composite.c`、`hanxin.c` 等）+ 输出模块（`bmp.c`、`png.c`、`gif.c`、`tif.c`、`svg.c`、`emf.c`、`pcx.c`、`ps.c`、`vector.c`、`raster.c`、`output.c`）+ 内嵌 `zlib.h`/`zconf.h`、`png.h`/`pngconf.h`；`fonts/` 资源；版本宏见 `zintconfig.h` |
| uthash | `include/uthash.h` | 无版本 | 哈希表单头实现（上游 `github.com:troydhanson/uthash.git`） |
| zlib | `zlib/` | 官网 zlib.net | 仅 `build.txt`（`./configure --prefix=../` + 改 Makefile 工具链）；产物被 gitignore |
| libpng | `img/png/` | 1.6.45（`.gitignore` 另含 1.4.22 痕迹） | 仅 `build.txt`（依赖 `../../../zlib/include`、`../../../zlib/lib`）；源码/产物被 gitignore |
| wpa_supplicant | `netopt/wpas/` | 2.10 | 仅裁剪 `wpa_ctrl.c/.h` 等（集成宏见 netopt 节） |

## 三、示例与验证代码

- `netopt/example/`：
  - `Makefile`：默认目标 `pro_wifi`，交叉编译 `test_wifi.c` + `../wifi_opt.c` + `../wpas/os_unix.c` + `../wpas/wpa_ctrl.c` + `../../file/file_opt.c` + `../net_opt.c` + `../net_netlink.c`（`CC` 取 A8 devkit `arm-linux-gnueabihf-gcc` 等）；`install` 目标 `cp pro_wifi ~/nfs`。
  - `test_wifi.c`：演示 `wifi_sta_enable` → `wifi_sta_scan` → `wifi_sta_connect`。
  - `test_gate.c`：演示 `nl_getGateways` / `replace_gateway` / `del_gateway`。
- 已知现状：`netopt/example/pro_wifi` 为编译产出的 aarch64 二进制（未纳入版本控制）；`Makefile` 中引用的部分测试文件（`test.c`、`test_nl.c`、`test_dns.c`、`test_link.c`、`test_dhcp.c`、`tcpIp/multicast.c`、`tcpIp/raw_socket.c`）在仓库中不存在。
