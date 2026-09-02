#include "applefiledrag.h"

#include <utility>

void AppleRemoteFileDragGate::update(const AppleRemoteFileDrag& drag)
{
    if (drag.sessionId == 0 || drag.sourcePaths.isEmpty()) {
        clear();
        return;
    }
    m_Pending = drag;
}

void AppleRemoteFileDragGate::clear()
{
    m_Pending.reset();
}

bool AppleRemoteFileDragGate::hasPending() const
{
    return m_Pending.has_value();
}

std::optional<AppleRemoteFileDrag> AppleRemoteFileDragGate::takeIfEligible(
        bool leftButtonDown,
        bool pointerInsideStream)
{
    if (!m_Pending.has_value() || !leftButtonDown || pointerInsideStream) {
        return std::nullopt;
    }
    std::optional<AppleRemoteFileDrag> result = std::move(m_Pending);
    m_Pending.reset();
    return result;
}

AppleRemoteFileDragInputTransition
AppleRemoteFileDragInputState::nativeDragBegan(quint8 buttons)
{
    m_NativeDragOwnsRelease = true;
    return {static_cast<quint8>(buttons & ~quint8(1)), false};
}

AppleRemoteFileDragInputTransition
AppleRemoteFileDragInputState::nativeDragEnded(quint8 buttons)
{
    // AppKit owns the mouse-up after a Finder promise drag; emitting it at the
    // last remote point would complete a second drop on the remote Mac.
    // Windows can also publish SDL's queued button-up after DoDragDrop returns,
    // so ownership remains armed until that stale event is consumed or a new
    // physical press begins. Ending the native drag never emits a wire event.
    return {static_cast<quint8>(buttons & ~quint8(1)), false};
}

AppleRemoteFileDragInputTransition
AppleRemoteFileDragInputState::nativeDragStartFailed(quint8 buttons)
{
    m_NativeDragOwnsRelease = false;
    return {static_cast<quint8>(buttons | quint8(1)), false};
}

AppleRemoteFileDragInputTransition
AppleRemoteFileDragInputState::localLeftButtonChanged(
        bool pressed,
        quint8 buttons)
{
    if (pressed) {
        m_NativeDragOwnsRelease = false;
        return {static_cast<quint8>(buttons | quint8(1)), true};
    }
    const bool forward = !m_NativeDragOwnsRelease;
    m_NativeDragOwnsRelease = false;
    return {static_cast<quint8>(buttons & ~quint8(1)), forward};
}

void AppleRemoteFileDragInputState::reset()
{
    m_NativeDragOwnsRelease = false;
}

AppleLocalFileDragLifecycle::AppleLocalFileDragLifecycle(
        AcceptPoint acceptPoint,
        Announce announce,
        Pointer pointer,
        Cancel cancel)
    : m_AcceptPoint(std::move(acceptPoint)),
      m_Announce(std::move(announce)),
      m_Pointer(std::move(pointer)),
      m_Cancel(std::move(cancel))
{
}

bool AppleLocalFileDragLifecycle::enter(
        quintptr dragIdentity,
        const QStringList& paths,
        const AppleFileDragPoint& point)
{
    if (m_Active) {
        if (dragIdentity != 0 && dragIdentity == m_DragIdentity &&
                paths == m_Paths) {
            return move(point);
        }
        cancel();
    }
    if (dragIdentity == 0 || paths.isEmpty() || !accepts(point) ||
            !m_Announce || !m_Pointer ||
            !m_Announce(paths)) {
        return false;
    }
    m_DragIdentity = dragIdentity;
    m_Paths = paths;
    m_LastPoint = point;
    m_Active = true;
    m_Inside = true;
    m_Pointer(m_LastPoint, AppleLocalFileDragPointerAction::Press);
    return true;
}

bool AppleLocalFileDragLifecycle::move(const AppleFileDragPoint& point)
{
    if (!m_Active || !m_Pointer) return false;
    if (!accepts(point)) {
        m_Inside = false;
        return false;
    }
    m_Inside = true;
    if (point.x == m_LastPoint.x && point.y == m_LastPoint.y &&
            point.displayIndex == m_LastPoint.displayIndex) {
        return true;
    }
    m_LastPoint = point;
    m_Pointer(m_LastPoint, AppleLocalFileDragPointerAction::Move);
    return true;
}

bool AppleLocalFileDragLifecycle::drop(const AppleFileDragPoint& point)
{
    if (!m_Active) return false;
    if (!accepts(point)) {
        cancel();
        return false;
    }
    m_LastPoint = point;
    clear();
    if (m_Pointer) {
        m_Pointer(m_LastPoint, AppleLocalFileDragPointerAction::Release);
    }
    return true;
}

void AppleLocalFileDragLifecycle::leave()
{
    if (!m_Active) return;
    // Leaving a native view is not the end of the system drag. The platform
    // adapter owns detection of the eventual button release and calls cancel()
    // only if the drag ends outside every registered target.
    m_Inside = false;
}

void AppleLocalFileDragLifecycle::cancel()
{
    if (!m_Active) return;
    const AppleFileDragPoint lastPoint = m_LastPoint;
    clear();
    if (m_Cancel) m_Cancel();
    if (m_Pointer) {
        m_Pointer(lastPoint, AppleLocalFileDragPointerAction::Release);
    }
}

bool AppleLocalFileDragLifecycle::isActive() const
{
    return m_Active;
}

bool AppleLocalFileDragLifecycle::accepts(
        const AppleFileDragPoint& point) const
{
    return !m_AcceptPoint || m_AcceptPoint(point);
}

void AppleLocalFileDragLifecycle::clear()
{
    m_Active = false;
    m_Inside = false;
    m_DragIdentity = 0;
    m_Paths.clear();
}
