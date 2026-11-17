SOURCES += $$PWD/mqttPahocClient.c
    #$$PWD/taskMqtt$$PWD.cpp
HEADERS += $$PWD/mqttClient.h \
    $$PWD/mqttPahocClient.h
    #$$PWD/taskMqtt$$PWD.h \
    #$$PWD/taskMqttMeta.h
INCLUDEPATH += $$PWD \
    $$PWD/include
unix:!macx: LIBS += -L$$PWD/lib -lpaho-mqtt3c
