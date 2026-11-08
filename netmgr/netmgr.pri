# netmgr 网络管理服务：以太网 / WiFi（STA、AP）/ 4G 链路管理与优先级路由
#
# 分层组织，上层依赖下层，下层不反向依赖上层，可独立编入宿主工程：
#   common   类型 / 错误码 / 通用校验 / 进程控制（无横向依赖）
#   sys      系统适配：接口操作、路由、DHCP、DNS、wpa（STA 控制）、ap（hostapd+udhcpd）、netlink 事件
#   core     仲裁编排：链路实现（以太网 / WiFi / 4G）、状态判定、优先级选举、路由与 DNS 同步
#   api      对外接口：net_mgr.h，宿主工程只包含它
#
# 默认编入全部分层；宿主若只需下层能力，可先设置 NETMGR_LEVEL 再包含本文件：
#   NETMGR_LEVEL = sys      仅 common + sys
#   NETMGR_LEVEL = core     common + sys + core（不含 api）
#   NETMGR_LEVEL = all      全部分层（默认）
contains (DEFINES, M_NETMGR) {
isEmpty(NETMGR_LEVEL): NETMGR_LEVEL = all

NETMGR_WITH_API = 1
NETMGR_WITH_CORE = 1
equals(NETMGR_LEVEL, core) {
    NETMGR_WITH_API = 0
}
equals(NETMGR_LEVEL, sys) {
    NETMGR_WITH_API = 0
    NETMGR_WITH_CORE = 0
}

# ===== common 层：无横向依赖，任何层次都需编入（含进程控制，供 sys 层自管外部进程） =====
SOURCES += $$PWD/common/net_mgr_util.c \
    $$PWD/common/net_mgr_proc.c
HEADERS += $$PWD/common/net_mgr_limits.h \
    $$PWD/common/net_mgr_err.h \
    $$PWD/common/net_mgr_util.h \
    $$PWD/common/net_mgr_proc.h
INCLUDEPATH += $$PWD/common

# ===== sys 层：只依赖 common，可独立使用 =====
SOURCES += $$PWD/sys/net_mgr_iface.c \
    $$PWD/sys/net_mgr_route.c \
    $$PWD/sys/net_mgr_dhcp.c \
    $$PWD/sys/net_mgr_dns.c \
    $$PWD/sys/net_mgr_wpa.c \
    $$PWD/sys/net_mgr_ap.c \
    $$PWD/sys/net_mgr_event.c
HEADERS += $$PWD/sys/net_mgr_iface.h \
    $$PWD/sys/net_mgr_route.h \
    $$PWD/sys/net_mgr_dhcp.h \
    $$PWD/sys/net_mgr_dns.h \
    $$PWD/sys/net_mgr_wpa.h \
    $$PWD/sys/net_mgr_ap.h \
    $$PWD/sys/net_mgr_event.h
INCLUDEPATH += $$PWD/sys

# ===== core 层：依赖 common + sys，内含三条链路实现与就绪判定 =====
equals(NETMGR_WITH_CORE, 1) {
    SOURCES += $$PWD/core/net_mgr_core.c \
    $$PWD/core/net_mgr_arbiter.c \
        $$PWD/core/net_mgr_link.c \
        $$PWD/core/net_mgr_eth.c \
        $$PWD/core/net_mgr_wifi.c \
        $$PWD/core/net_mgr_cell.c
    HEADERS += $$PWD/core/net_mgr_core.h \
    $$PWD/core/net_mgr_arbiter.h \
        $$PWD/core/net_mgr_core_int.h
    INCLUDEPATH += $$PWD/core
}

# ===== api 层：对外接口，依赖 core =====
equals(NETMGR_WITH_API, 1) {
    SOURCES += $$PWD/api/net_mgr.c
    HEADERS += $$PWD/api/net_mgr.h
    INCLUDEPATH += $$PWD/api
}

unix:!macx: LIBS += -lpthread
}
