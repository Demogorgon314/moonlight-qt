#pragma once

#include <QByteArray>
#include <QString>

namespace MacClipboard {

bool readImageAsPng(QByteArray& outPng,
                    QString* outSourceDescription = nullptr,
                    QString* outFormatsSummary = nullptr,
                    bool* outHadImageLikeData = nullptr);

bool writeImageFromPng(const QByteArray& png,
                       QString* outWriteDescription = nullptr);

int changeCount();

} // namespace MacClipboard