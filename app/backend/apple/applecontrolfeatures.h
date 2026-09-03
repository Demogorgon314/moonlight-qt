#pragma once

#include "applekeyboardinputsource.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QPoint>
#include <QSize>
#include <QSet>
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
    QList<QSet<quint32>> availableKeySymbolUpdates;
    QList<AppleKeyboardInputSourceState> keyboardInputSourceUpdates;

    bool isEmpty() const
    {
        return cursorUpdates.isEmpty() && displayLayouts.isEmpty() &&
                availableKeySymbolUpdates.isEmpty() &&
                keyboardInputSourceUpdates.isEmpty();
    }
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

struct AppleClipboardTypeAlias
{
    QString tagClass;
    QString preferredTag;

    bool operator==(const AppleClipboardTypeAlias& other) const
    {
        return tagClass == other.tagClass &&
                preferredTag == other.preferredTag;
    }
};

struct AppleClipboardFlavor
{
    QString type;
    QList<AppleClipboardTypeAlias> aliases;
    QByteArray value;
    quint32 reserved = 0;

    bool operator==(const AppleClipboardFlavor& other) const
    {
        return type == other.type && aliases == other.aliases &&
                value == other.value && reserved == other.reserved;
    }
};

struct AppleClipboardItem
{
    QList<AppleClipboardFlavor> flavors;

    bool operator==(const AppleClipboardItem& other) const
    {
        return flavors == other.flavors;
    }
};

struct AppleClipboardArchive
{
    QList<AppleClipboardItem> items;

    bool operator==(const AppleClipboardArchive& other) const
    {
        return items == other.items;
    }

    bool isEmpty() const { return items.isEmpty(); }
    std::optional<QString> preferredText() const;
    static AppleClipboardArchive text(const QString& text);
};

struct AppleClipboardResult
{
    bool consumed = false;
    bool receivedAutomatically = false;
    QList<QByteArray> outboundMessages;
    std::optional<AppleClipboardArchive> receivedArchive;
};

// Owns the promise/data exchange and encrypted-record reassembly used by the
// native shared pasteboard protocol. Automatic responses are guarded by an
// eligibility generation so a clipboard fetched for an inactive window is
// never installed later.
class AppleClipboardExchange
{
public:
    static constexpr int MaximumCompressedBytes = 7 * 1024 * 1024;
    static constexpr int MaximumArchiveBytes = 64 * 1024 * 1024;
    static constexpr int FragmentBytes = 32 * 1024;
    static constexpr qint64 RequestLifetimeMilliseconds = 120 * 1000;

    QList<QByteArray> setSharingEnabled(bool enabled);
    void setAutomaticEligible(bool eligible);
    QList<QByteArray> advertiseLocalArchive(
            const AppleClipboardArchive& archive,
            QString* error = nullptr);
    QList<QByteArray> requestRemoteClipboard(qint64 nowMilliseconds = 0);
    AppleClipboardResult receive(const QByteArray& message,
                                 QString* error = nullptr,
                                 qint64 nowMilliseconds = 0);
    void resetForReconnect();

    static QByteArray request(bool promises, quint32 sessionId);
    static QList<QByteArray> encodeArchive(
            const AppleClipboardArchive& archive,
            bool promises = false,
            quint32 sessionId = 0,
            QString* error = nullptr);
    static QList<QByteArray> encodeText(const QString& text,
                                        bool promises = false,
                                        quint32 sessionId = 0,
                                        QString* error = nullptr);

private:
    enum class RequestState
    {
        Idle,
        AwaitingAutomaticPromises,
        AwaitingAutomaticData,
        AwaitingManualData,
    };

    static bool decodeEnvelope(const QByteArray& message,
                               quint32* sessionId,
                               bool* containsPromises,
                               AppleClipboardArchive* archive,
                               QString* error);
    static QList<QByteArray> fragments(const QByteArray& message);
    void beginRequest(RequestState state,
                      quint32 sessionId,
                      qint64 nowMilliseconds);
    void resetRequest();
    void expireRequest(qint64 nowMilliseconds);
    quint32 nextSessionId();

    QByteArray m_Reassembly;
    int m_ExpectedLength = 0;
    AppleClipboardArchive m_LocalArchive;
    quint32 m_RequestSessionId = 0;
    quint32 m_NextSessionId = 1;
    quint64 m_EligibilityGeneration = 0;
    quint64 m_RequestEligibilityGeneration = 0;
    qint64 m_RequestDeadlineMilliseconds = -1;
    RequestState m_RequestState = RequestState::Idle;
    bool m_SharingEnabled = false;
    bool m_SharingStateKnown = false;
    bool m_AutomaticEligible = false;
    bool m_HasUnfulfilledPromises = false;
};

// Keeps the local clipboard snapshot used by the Apple promise exchange. The
// stream window is owned by SDL, so QClipboard notifications alone are not a
// sufficient boundary when focus moves between native applications.
class AppleLocalClipboardTracker
{
public:
    std::optional<AppleClipboardArchive> dataChanged(
            const std::optional<AppleClipboardArchive>& archive);
    std::optional<AppleClipboardArchive> windowFocusGained(
            const std::optional<AppleClipboardArchive>& archive);
    void expectRemoteArchive(const AppleClipboardArchive& archive);
    void reset();
    static bool containsFiles(const QMimeData* mime);

private:
    std::optional<AppleClipboardArchive> observe(
            const std::optional<AppleClipboardArchive>& archive);

    std::optional<AppleClipboardArchive> m_PendingRemoteArchive;
    std::optional<AppleClipboardArchive> m_LastObservedArchive;
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
