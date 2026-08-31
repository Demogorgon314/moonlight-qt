QT += core network
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = apple_protocol_core

INCLUDEPATH += \
    ../../app \
    ../../libs/windows/include/x64 \
    ../../libs/windows/include

win32 {
    LIBS += -L../../libs/windows/lib/x64 -llibcrypto -lavcodec -lavutil -lswscale -ladvapi32 -lws2_32
}

SOURCES += \
    main.cpp \
    ../../app/backend/apple/applefeaturegate.cpp \
    ../../app/backend/apple/appleconnectionstore.cpp \
    ../../app/backend/apple/applecredentialstore.cpp \
    ../../app/backend/apple/appleprotocol.cpp \
    ../../app/backend/apple/appleauthenticator.cpp \
    ../../app/backend/apple/applemediatransport.cpp \
    ../../app/backend/apple/applemediaprotocol.cpp \
    ../../app/backend/apple/applevideodecoder.cpp

HEADERS += \
    ../../app/backend/apple/applefeaturegate.h \
    ../../app/backend/apple/appleconnectionstore.h \
    ../../app/backend/apple/applecredentialstore.h \
    ../../app/backend/apple/appleprotocol.h \
    ../../app/backend/apple/appleauthenticator.h \
    ../../app/backend/apple/applemediatransport.h \
    ../../app/backend/apple/applemediaprotocol.h \
    ../../app/backend/apple/applevideodecoder.h
