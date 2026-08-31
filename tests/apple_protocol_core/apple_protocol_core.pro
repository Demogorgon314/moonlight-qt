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
    LIBS += -L../../libs/windows/lib/x64 -llibcrypto -ladvapi32
}

SOURCES += \
    main.cpp \
    ../../app/backend/apple/applefeaturegate.cpp \
    ../../app/backend/apple/appleconnectionstore.cpp \
    ../../app/backend/apple/applecredentialstore.cpp \
    ../../app/backend/apple/appleprotocol.cpp \
    ../../app/backend/apple/appleauthenticator.cpp

HEADERS += \
    ../../app/backend/apple/applefeaturegate.h \
    ../../app/backend/apple/appleconnectionstore.h \
    ../../app/backend/apple/applecredentialstore.h \
    ../../app/backend/apple/appleprotocol.h \
    ../../app/backend/apple/appleauthenticator.h
