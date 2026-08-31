QT += core gui quick
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = multiprotocol_architecture

INCLUDEPATH += ../../app

SOURCES += \
    main.cpp \
    ../../app/backend/protocol/protocoltypes.cpp \
    ../../app/backend/protocol/resolvedlaunchplan.cpp \
    ../../app/streaming/streamsession.cpp

HEADERS += \
    ../../app/backend/protocol/protocoltypes.h \
    ../../app/backend/protocol/resolvedlaunchplan.h \
    ../../app/streaming/streamsession.h

