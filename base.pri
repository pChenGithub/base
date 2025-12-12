# mongoose
contains (DEFINES, M_MONGOOSE) {
SOURCES += $$PWD/mongoose/mongoose.c \
    $$PWD/mongoose/webserver.c
HEADERS += $$PWD/mongoose/mongoose.h \
    $$PWD/mongoose/webserver.h \
    $$PWD/mongoose/web_config.h
INCLUDEPATH += $$PWD/mongoose
}
# cjson
contains (DEFINES, M_CJSON) {
SOURCES += $$PWD/json/cJSON.c \
    $$PWD/json/cJSON_tools.c
HEADERS += $$PWD/json/cJSON.h \
    $$PWD/json/cJSON_tools.h \
    $$PWD/json/json_errno.h
INCLUDEPATH += $$PWD/json
}
# mqtt
contains (DEFINES, M_MQTT) {
SOURCES += $$PWD/mqtt/mqttPahocClient.c \
    #$$PWD/mqtt/taskMqttBase.cpp

HEADERS += $$PWD/mqtt/mqttClient.h \
    $$PWD/mqtt/mqttPahocClient.h \
    #$$PWD/mqtt/taskMqttBase.h \
    #$$PWD/mqtt/taskMqttMeta.h

INCLUDEPATH += $$PWD/mqtt \
    $$PWD/mqtt/include
unix:!macx: LIBS += -L$$PWD/mqtt/lib -lpaho-mqtt3c
}
# mixshell
contains (DEFINES, M_MIXSHELL) {
SOURCES += $$PWD/misc/mixShell.c
HEADERS += $$PWD/misc/mixShell.h
INCLUDEPATH *= $$PWD/misc
}
# debug opt
contains (DEFINES, M_DEBUG) {
SOURCES += $$PWD/misc/debug_opt.c
HEADERS += $$PWD/misc/debug_opt.h
INCLUDEPATH *= $$PWD/misc
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
SOURCES += $$PWD/curl/ifaceHttpReq.c
HEADERS += $$PWD/curl/ifaceHttpReq.h
INCLUDEPATH += $$PWD/curl
unix:!macx: LIBS += -L$$PWD/curl/lib -lcurl
unix:!macx: LIBS += -L$$PWD/openssl/buid_aarch64/lib -lssl -lcrypto
}
# log
contains (DEFINES, M_LOG) {
SOURCES += $$PWD/zlog/lib_log.c
HEADERS += $$PWD/zlog/lib_log.h
INCLUDEPATH += $$PWD/zlog
unix:!macx: LIBS += -L$$PWD/zlog/lib -lzlog
}

