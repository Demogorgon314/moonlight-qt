#pragma once

#include <QHash>
#include <QList>
#include <QVector>
#include <QtGlobal>

#include <optional>

struct AppleRemoteKeyEvent
{
    bool isDown = false;
    quint32 symbol = 0;
    quint16 keyboardType = 0;
    quint16 keyCode = 0;
};

// Translates SDL's cross-platform keyboard events into the native tuple used
// by Apple Screen Sharing. The mapper also owns pressed-key state so focus loss
// can release the exact remote keys that were pressed, even if the local layout
// changes between key-down and key-up.
class AppleKeyboardMapper
{
public:
    explicit AppleKeyboardMapper(bool swapAltAndCommand,
                                 quint16 keyboardType = 0);

    std::optional<AppleRemoteKeyEvent> update(bool isDown,
                                               int sdlKeycode,
                                               int sdlScancode,
                                               bool systemKeyCaptureRequested);
    QList<AppleRemoteKeyEvent> updateWithModifiers(
            bool isDown,
            int sdlKeycode,
            int sdlScancode,
            int sdlModifiers,
            int platformGuiModifiers,
            bool systemKeyCaptureRequested);
    QList<AppleRemoteKeyEvent> releaseAll();
    int pressedKeyCount() const;

private:
    struct PressedKey
    {
        quint32 symbol = 0;
        quint16 keyCode = 0;
    };

    bool m_SwapAltAndCommand = false;
    quint16 m_KeyboardType = 0;
    QHash<int, PressedKey> m_PressedKeys;
    QVector<int> m_PressOrder;
    int m_SynthesizedGuiScancode = 0;
};
