SOURCES += $$PWD/lib_log.c
SOURCES += $$PWD/lib_logTool.c
HEADERS += $$PWD/lib_log.h
HEADERS += $$PWD/lib_logTool.h
INCLUDEPATH += $$PWD
unix:!macx: LIBS += -L$$PWD/lib -lzlog
