# base 项目模块明细

- 更新日期: 2026-09-18
- 状态: 已初始化（自 `PROJECT.md` 拆分）
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
