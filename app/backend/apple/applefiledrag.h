#pragma once

#include "applefiletransfer.h"

#include <QStringList>

#include <functional>
#include <optional>

struct AppleFileDragPoint
{
    int x = 0;
    int y = 0;
    int displayIndex = 0;
};

enum class AppleLocalFileDragPointerAction
{
    Press,
    Move,
    Release,
};

// Keeps a Mac-originated drag inert while it remains inside the streamed
// desktop. Native viewers only export the promised files after the pointer
// crosses into the local desktop with the drag button still held.
class AppleRemoteFileDragGate
{
public:
    void update(const AppleRemoteFileDrag& drag);
    void clear();
    bool hasPending() const;
    std::optional<AppleRemoteFileDrag> takeIfEligible(
            bool leftButtonDown,
            bool pointerInsideStream);

private:
    std::optional<AppleRemoteFileDrag> m_Pending;
};

struct AppleRemoteFileDragInputTransition
{
    quint8 buttons = 0;
    bool forwardToRemote = false;
};

// Owns the physical left-button release while a Mac-originated promise is a
// native local drag. The native drag loop ends locally; forwarding that end as
// another remote mouse-up completes a duplicate Finder drop on the Mac.
class AppleRemoteFileDragInputState
{
public:
    AppleRemoteFileDragInputTransition nativeDragBegan(quint8 buttons);
    AppleRemoteFileDragInputTransition nativeDragEnded(quint8 buttons);
    AppleRemoteFileDragInputTransition nativeDragStartFailed(quint8 buttons);
    AppleRemoteFileDragInputTransition localLeftButtonChanged(
            bool pressed,
            quint8 buttons);
    void reset();

private:
    bool m_NativeDragOwnsRelease = false;
};

// Platform drop targets feed this lifecycle so the wire announcement always
// precedes the remote press and one system drag keeps one remote offer even
// when the pointer briefly crosses another native window.
class AppleLocalFileDragLifecycle
{
public:
    using AcceptPoint = std::function<bool(const AppleFileDragPoint&)>;
    using Announce = std::function<bool(const QStringList&)>;
    using Pointer = std::function<void(
            const AppleFileDragPoint&,
            AppleLocalFileDragPointerAction)>;
    using Cancel = std::function<void()>;

    AppleLocalFileDragLifecycle(AcceptPoint acceptPoint,
                                Announce announce,
                                Pointer pointer,
                                Cancel cancel);

    // dragIdentity is the platform drag object's stable identity. Paths alone
    // cannot distinguish a re-entry from a later drag of the same file.
    bool enter(quintptr dragIdentity,
               const QStringList& paths,
               const AppleFileDragPoint& point);
    bool move(const AppleFileDragPoint& point);
    bool drop(const AppleFileDragPoint& point);
    // Leaving a view does not end the system drag. The platform adapter must
    // call cancel() when it observes that the drag button was released outside.
    void leave();
    void cancel();
    bool isActive() const;

private:
    bool accepts(const AppleFileDragPoint& point) const;
    void clear();

    AcceptPoint m_AcceptPoint;
    Announce m_Announce;
    Pointer m_Pointer;
    Cancel m_Cancel;
    quintptr m_DragIdentity = 0;
    QStringList m_Paths;
    AppleFileDragPoint m_LastPoint;
    bool m_Active = false;
    bool m_Inside = false;
};
