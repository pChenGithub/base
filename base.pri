# mongoose
contains (DEFINES, M_MONGOOSE) {
include ($$PWD/mongoose/mongoose.pri)
}
# cjson
contains (DEFINES, M_CJSON) {
include ($$PWD/json/json.pri)
}
# mqtt
contains (DEFINES, M_MQTT) {
include ($$PWD/mqtt/mqtt.pri)
}
# mixshell
contains (DEFINES, M_MIXSHELL) {
include ($$PWD/misc/shell.pri)
}
# debug opt
contains (DEFINES, M_DEBUG) {
include ($$PWD/misc/debug.pri)
}
# string opt
contains (DEFINES, M_STRING) {
SOURCES += $$PWD/string/string_opt.c
HEADERS += $$PWD/string/string_opt.h
INCLUDEPATH += $$PWD/string
}
# file opt
contains (DEFINES, M_FILE) {
SOURCES += $$PWD/file/file_opt.c
HEADERS += $$PWD/file/file_opt.h
INCLUDEPATH += $$PWD/file
}
# time opt
contains (DEFINES, M_TIME) {
SOURCES += $$PWD/timeopt/timeopt.c
HEADERS += $$PWD/timeopt/timeopt.h
INCLUDEPATH += $$PWD/timeopt
}
# http client
contains (DEFINES, M_HTTPC) {
include ($$PWD/curl/httpc.pri)
}
# log
contains (DEFINES, M_LOG) {
include ($$PWD/zlog/log.pri)
}
# net opt
contains (DEFINES, M_NET) {
include ($$PWD/netopt/net.pri)
}
# mmc
contains (DEFINES, M_MMC) {
include ($$PWD/misc/mmc.pri)
}
# net mgr（网络管理服务，内部按 M_NETMGR 判断是否编入）
include ($$PWD/netmgr/netmgr.pri)
# modbus
contains(DEFINES, M_MODBUS) {
include ($$PWD/modbus/modbus.pri)
}
# zint
contains(DEFINES, M_ZINT) {
include ($$PWD/zint/zint.pri)
}

