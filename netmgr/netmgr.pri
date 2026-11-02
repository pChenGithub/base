# netmgr 网络管理服务：以太网 / WiFi（STA、AP）/ 4G 链路管理与优先级路由
# 本模块不依赖 base 内其它自研模块，可独立编入宿主工程
contains (DEFINES, M_NETMGR) {
SOURCES += $$PWD/net_mgr.c \
    $$PWD/net_mgr_link.c \
    $$PWD/net_mgr_proc.c \
    $$PWD/net_mgr_dhcp.c \
    $$PWD/net_mgr_dns.c \
    $$PWD/net_mgr_eth.c \
    $$PWD/net_mgr_wifi.c \
    $$PWD/net_mgr_cell.c \
    $$PWD/net_mgr_route.c \
    $$PWD/net_mgr_event.c

HEADERS += $$PWD/net_mgr.h \
    $$PWD/net_mgr_def.h \
    $$PWD/net_mgr_err.h

INCLUDEPATH += $$PWD
unix:!macx: LIBS += -lpthread
}
