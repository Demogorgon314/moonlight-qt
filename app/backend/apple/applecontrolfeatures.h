#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSize>
#include <QString>

#include <optional>

struct AppleCursorImage
{
    int width = 0;
    int height = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    QByteArray rgba;

    bool isUsable() const;
};

struct AppleCursorUpdate
{
    enum class Kind
    {
        Store,
        Select,
    };

    Kind kind = Kind::Select;
    quint32 id = 0;
    AppleCursorImage image;
};

struct AppleDisplayRect
{
    quint32 id = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int logicalX = 0;
    int logicalY = 0;
    int logicalWidth = 0;
    int logicalHeight = 0;
};

struct AppleDisplayLayout
{
    int scaledWidth = 0;
    int scaledHeight = 0;
    int backingWidth = 0;
    int backingHeight = 0;
    QList<AppleDisplayRect> displays;

    bool isUsable() const;
};

struct AppleControlEvents
{
    QList<AppleCursorUpdate> cursorUpdates;
    QList<AppleDisplayLayout> displayLayouts;
};

// Parses the variable-length rectangles carried by one encrypted framebuffer
// update. Malformed trailing rectangles do not erase earlier valid events.
class AppleControlEventParser
{
public:
    static AppleControlEvents parse(const QByteArray& message);
};

namespace AppleDynamicResolution {

// Matches the Mac client's single-display policy: preserve aspect ratio,
// constrain the virtual display to 1920x1080, request at least 320x200 when
// the aspect ratio permits it, and keep HEVC dimensions even.
QSize normalizedSize(int width, int height);

} // namespace AppleDynamicResolution

struct AppleTextClipboardResult
{
    bool consumed = false;
    QList<QByteArray> outboundMessages;
    std::optional<QString> receivedText;
};

// Owns the promise/data exchange and encrypted-record reassembly used by the
// native shared pasteboard protocol. Its interface deliberately exposes only
// UTF-8 text; file URLs and every non-text flavor remain unreachable.
class AppleTextClipboardExchange
{
public:
    static constexpr int MaximumCompressedBytes = 7 * 1024 * 1024;
    static constexpr int MaximumArchiveBytes = 64 * 1024 * 1024;
    static constexpr int FragmentBytes = 60000;

    QList<QByteArray> setEligible(bool eligible);
    QList<QByteArray> advertiseLocalText(const QString& text,
                                         QString* error = nullptr);
    AppleTextClipboardResult receive(const QByteArray& message,
                                     QString* error = nullptr);
    void resetForReconnect();

    static QByteArray request(bool promises, quint32 sessionId);
    static QList<QByteArray> encodeText(const QString& text,
                                        bool promises = false,
                                        quint32 sessionId = 0,
                                        QString* error = nullptr);

private:
    enum class RequestState
    {
        Idle,
        AwaitingPromises,
        AwaitingData,
    };

    static bool decodeEnvelope(const QByteArray& message,
                               quint32* sessionId,
                               bool* containsPromises,
                               std::optional<QString>* text,
                               QString* error);
    static QList<QByteArray> fragments(const QByteArray& message);
    quint32 nextSessionId();

    QByteArray m_Reassembly;
    int m_ExpectedLength = 0;
    QString m_LocalText;
    quint32 m_RequestSessionId = 0;
    quint32 m_NextSessionId = 1;
    RequestState m_RequestState = RequestState::Idle;
    bool m_Eligible = false;
    bool m_HasLocalText = false;
};
