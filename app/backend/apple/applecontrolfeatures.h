#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QSize>
#include <QString>
#include <QStringList>

#include <optional>

class QMimeData;

struct AppleCursorImage
{
    int width = 0;
    int height = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    QByteArray rgba;

    bool isUsable() const;
    AppleCursorImage scaledForDpi(double scale) const;
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

// Mirrors the host's cursor cache while keeping the selected source image
// independent from any platform cursor created for a particular display DPI.
class AppleCursorStore
{
public:
    static constexpr int MaximumEntries = 64;

    std::optional<AppleCursorImage> apply(const AppleCursorUpdate& update);
    std::optional<AppleCursorImage> selectedImage() const;
    std::optional<quint32> selectedId() const;
    void clear();

private:
    QHash<quint32, AppleCursorImage> m_Cache;
    QList<quint32> m_CacheOrder;
    std::optional<AppleCursorImage> m_SelectedImage;
    std::optional<quint32> m_SelectedId;
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
QSize normalizedSizeForDpi(int width, int height, double dpiScale);
QSize initialDisplaySize(const std::optional<QSize>& storedViewport);

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

// Keeps the local clipboard snapshot used by the Apple promise exchange. The
// stream window is owned by SDL, so QClipboard notifications alone are not a
// sufficient boundary when focus moves between native applications.
class AppleLocalClipboardTracker
{
public:
    std::optional<QString> dataChanged(const QMimeData* mime);
    std::optional<QString> windowFocusGained(const QMimeData* mime);
    void expectRemoteText(const QString& text);
    void reset();
    static bool containsFiles(const QMimeData* mime);

private:
    std::optional<QString> observe(const QMimeData* mime);

    std::optional<QString> m_PendingRemoteText;
    std::optional<QString> m_LastObservedText;
};

enum class ApplePerformanceOverlayStyle
{
    Moonlight = 0,
    Detailed = 1,
};

struct ApplePerformanceOverlayPolicy
{
    bool visible = false;
    ApplePerformanceOverlayStyle style =
            ApplePerformanceOverlayStyle::Moonlight;

    static ApplePerformanceOverlayPolicy fromSettings(
            bool showPerformanceOverlay,
            int styleValue);
    QPoint topLeft(const QSize& outputSize,
                   const QSize& overlaySize) const;
};

struct ApplePerformanceOverlayMetrics
{
    QSize canvasSize;
    double receivedFramesPerSecond = 0.0;
    double decodedFramesPerSecond = 0.0;
    double presentedFramesPerSecond = 0.0;
    double networkMegabitsPerSecond = 0.0;
    double decodeMilliseconds = 0.0;
    double renderMilliseconds = 0.0;
    QString decoderBackend;
    bool hasMediaSample = false;
    bool hasPresentationSample = false;
};

struct ApplePerformanceOverlayTextRun
{
    QString text;
    int pixelSize = 18;
    bool bold = false;
};

QList<ApplePerformanceOverlayTextRun> appleMoonlightPerformanceRuns(
        const ApplePerformanceOverlayMetrics& metrics);

QStringList appleMoonlightPerformanceLines(
        const ApplePerformanceOverlayMetrics& metrics);
