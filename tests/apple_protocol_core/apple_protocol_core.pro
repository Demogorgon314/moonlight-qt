QT += core gui network zlib-private
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = apple_protocol_core

INCLUDEPATH += \
    ../../app

win32 {
    DEFINES += APPLE_FILE_DRAG_TESTS
    INCLUDEPATH += \
        ../../libs/windows/include/x64 \
        ../../libs/windows/include/x64/SDL2 \
        ../../libs/windows/include
    LIBS += -L../../libs/windows/lib/x64 -llibcrypto -lavcodec -lavutil -lswscale -lSDL2 -ladvapi32 -ld3d11 -ldxgi -luser32 -lgdi32 -lws2_32 -lole32 -lshell32 -lshlwapi -luuid
    SOURCES += \
        ../../app/backend/apple/appled3d11renderer.cpp \
        ../../app/backend/apple/applefiledrag_win.cpp
    HEADERS += \
        ../../app/backend/apple/appled3d11renderer.h \
        ../../app/backend/apple/applefiledrag_win.h
}

macx {
    INCLUDEPATH += \
        ../../libs/mac/include \
        ../../libs/mac/include/SDL2
    LIBS += -L../../libs/mac/lib \
        -lcrypto.3 -lavcodec.63 -lavutil.61 -lswscale.10 -lSDL2 \
        -framework Security -framework AudioToolbox -framework CoreVideo \
        -framework Metal -framework QuartzCore -framework AppKit
    SOURCES += \
        ../../app/backend/apple/applemetalrenderer.mm \
        ../../app/backend/apple/appleaudiodecoder_macos.mm
    HEADERS += ../../app/backend/apple/applemetalrenderer.h
}

SOURCES += \
    main.cpp \
    ../../app/backend/apple/applefeaturegate.cpp \
    ../../app/backend/apple/appleconnectionstore.cpp \
    ../../app/backend/apple/applecredentialstore.cpp \
    ../../app/backend/apple/applekeyboardmapper.cpp \
    ../../app/backend/apple/appleprotocol.cpp \
    ../../app/backend/apple/applefiledrag.cpp \
    ../../app/backend/apple/applefiletransfer.cpp \
    ../../app/backend/apple/applefilecopy.cpp \
    ../../app/backend/apple/applefiletransferservice.cpp \
    ../../app/backend/apple/applecontrolfeatures.cpp \
    ../../app/backend/apple/appleaudiostream.cpp \
    ../../app/backend/apple/appleauthenticator.cpp \
    ../../app/backend/apple/applemediatransport.cpp \
    ../../app/backend/apple/applemediaprotocol.cpp \
    ../../app/backend/apple/applevideorenderer.cpp \
    ../../app/backend/apple/applevideodecoder.cpp \
    ../../app/backend/apple/applewindowplacement.cpp \
    ../../app/settings/devicelocalsettings.cpp

HEADERS += \
    ../../app/backend/apple/applefeaturegate.h \
    ../../app/backend/apple/appleconnectionstore.h \
    ../../app/backend/apple/applecredentialstore.h \
    ../../app/backend/apple/applekeyboardmapper.h \
    ../../app/backend/apple/appleprotocol.h \
    ../../app/backend/apple/applefiledrag.h \
    ../../app/backend/apple/applefiletransfer.h \
    ../../app/backend/apple/applefilecopy.h \
    ../../app/backend/apple/applefiletransferservice.h \
    ../../app/backend/apple/applecontrolfeatures.h \
    ../../app/backend/apple/appleaudiostream.h \
    ../../app/backend/apple/appleauthenticator.h \
    ../../app/backend/apple/applemediatransport.h \
    ../../app/backend/apple/applemediaprotocol.h \
    ../../app/backend/apple/applevideorenderer.h \
    ../../app/backend/apple/applevideodecoder.h \
    ../../app/backend/apple/applewindowplacement.h \
    ../../app/settings/devicelocalsettings.h

RESOURCES += d3d11_test_resources.qrc
