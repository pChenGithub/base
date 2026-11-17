SOURCES += $$PWD/ifaceHttpReq.c
HEADERS += $$PWD/ifaceHttpReq.h
INCLUDEPATH += $$PWD $$PWD/include
unix:!macx: LIBS += -L$$PWD/lib -lcurl
unix:!macx: LIBS += -L$$PWD/../openssl/buid_aarch64/lib -lssl -lcrypto
