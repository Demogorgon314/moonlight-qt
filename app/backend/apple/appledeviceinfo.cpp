#include "appledeviceinfo.h"

#include "appleprotocol.h"

#include <limits>

namespace {

constexpr int MaximumTxtBytes = 4096;

std::optional<QString> utf8String(const QByteArray& bytes)
{
    const QString value = QString::fromUtf8(bytes.constData(), bytes.size());
    return value.toUtf8() == bytes ? std::optional<QString>(value)
                                   : std::nullopt;
}

QString normalizedOptionalValue(const QString& value)
{
    return value.trimmed();
}

std::optional<QString> nulTerminatedString(const QByteArray& payload,
                                           int* offset,
                                           int length)
{
    if (offset == nullptr || length <= 0 || *offset < 0 ||
            *offset > payload.size() - length) {
        return std::nullopt;
    }
    const QByteArray bytes = payload.mid(*offset, length);
    *offset += length;
    if (bytes.back() != '\0' || bytes.left(bytes.size() - 1).contains('\0')) {
        return std::nullopt;
    }
    return utf8String(bytes.left(bytes.size() - 1));
}

std::optional<qint32> colorIndex(const QString& value)
{
    bool ok = false;
    const qlonglong parsed = value.trimmed().toLongLong(&ok);
    if (!ok || parsed < std::numeric_limits<qint32>::min() ||
            parsed > std::numeric_limits<qint32>::max()) {
        return std::nullopt;
    }
    return static_cast<qint32>(parsed);
}

} // namespace

std::optional<qint32> AppleRemoteDeviceInfo::enclosureColorIndex() const
{
    const std::optional<qint32> deviceIndex = colorIndex(deviceColor);
    return deviceIndex.has_value() ? deviceIndex : colorIndex(enclosureColor);
}

AppleRemoteDeviceInfo AppleDeviceInfo::fromTxtAttributes(
        const QMap<QByteArray, QByteArray>& attributes)
{
    QMap<QString, QString> values;
    int encodedSize = 0;
    for (auto it = attributes.cbegin(); it != attributes.cend(); ++it) {
        encodedSize += 1 + it.key().size() +
                (it.value().isNull() ? 0 : 1 + it.value().size());
        if (encodedSize > MaximumTxtBytes) {
            return {};
        }
        const std::optional<QString> key = utf8String(it.key());
        const std::optional<QString> value = utf8String(it.value());
        if (!key.has_value() || !value.has_value()) {
            return {};
        }
        values.insert(key->toLower(), *value);
    }

    AppleRemoteDeviceInfo info;
    info.modelIdentifier = values.value(QStringLiteral("model")).trimmed();
    info.deviceColor = normalizedOptionalValue(
            values.value(QStringLiteral("icolor")));
    info.enclosureColor = normalizedOptionalValue(
            values.value(QStringLiteral("ecolor")));
    return info.isValid() ? info : AppleRemoteDeviceInfo();
}

AppleRemoteDeviceInfo AppleDeviceInfo::fromWirePayload(
        const QByteArray& payload)
{
    if (payload.size() < 16) {
        return {};
    }
    bool ok = false;
    const quint16 version = AppleWire::readUInt16(payload, 0, &ok);
    if (!ok) return {};
    const int modelLength = AppleWire::readUInt16(payload, 10, &ok);
    if (!ok) return {};
    const int deviceColorLength = AppleWire::readUInt16(payload, 12, &ok);
    if (!ok) return {};
    const int enclosureColorLength = AppleWire::readUInt16(payload, 14, &ok);
    if (!ok || modelLength <= 0 || deviceColorLength <= 0 ||
            enclosureColorLength <= 0) {
        return {};
    }
    const int stringBytes = modelLength + deviceColorLength + enclosureColorLength;
    const int requiredSize = 16 + stringBytes + (version >= 2 ? 4 : 0);
    if (requiredSize > payload.size()) {
        return {};
    }

    int offset = 16;
    const std::optional<QString> model = nulTerminatedString(
            payload, &offset, modelLength);
    const std::optional<QString> deviceColor = nulTerminatedString(
            payload, &offset, deviceColorLength);
    const std::optional<QString> enclosureColor = nulTerminatedString(
            payload, &offset, enclosureColorLength);
    if (!model.has_value() || !deviceColor.has_value() ||
            !enclosureColor.has_value()) {
        return {};
    }

    AppleRemoteDeviceInfo info;
    info.modelIdentifier = model->trimmed();
    info.deviceColor = normalizedOptionalValue(*deviceColor);
    info.enclosureColor = normalizedOptionalValue(*enclosureColor);
    return info.isValid() ? info : AppleRemoteDeviceInfo();
}

QByteArray AppleDeviceInfo::queryName(const QString& serviceName,
                                      const QString& serviceDomain)
{
    if (serviceName.isEmpty()) {
        return {};
    }
    QString domain = serviceDomain.trimmed();
    if (domain.isEmpty()) {
        return {};
    }
    if (!domain.endsWith(QLatin1Char('.'))) {
        domain.append(QLatin1Char('.'));
    }
    return serviceName.toUtf8() + QByteArrayLiteral("._device-info._tcp.") +
            domain.toUtf8();
}
