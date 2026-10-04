/*
    SPDX-FileCopyrightText: 2026 The KDE Contributors

    SPDX-License-Identifier: LGPL-2.1-or-later OR LGPL-3.0-or-later OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QBuffer>
#include <QImage>
#include <QImageIOPlugin>
#include <QImageWriter>
#include <QList>
#include <QVariant>
#include <QtEndian>

#include <optional>

namespace
{
constexpr QByteArrayView jpsFormat = "jps";
constexpr QByteArrayView mpoFormat = "mpo";
constexpr QByteArrayView stereoLayoutKey = "StereoLayout";
constexpr QByteArrayView jpsLayout = "side-by-side; right eye first";
constexpr QByteArrayView mpoLayout = "multi-picture object; left eye first";

bool isFormat(const QByteArray &format, QByteArrayView expected)
{
    return format.compare(expected, Qt::CaseInsensitive) == 0;
}

struct MpfEntry {
    quint32 attributes = 0;
    quint32 size = 0;
    quint32 offset = 0;
};

struct MpfData {
    qsizetype tiffOffset = -1;
    QList<MpfEntry> entries;
};

quint16 read16(const QByteArray &data, qsizetype offset, bool littleEndian)
{
    if (offset < 0 || offset + 2 > data.size()) {
        return 0;
    }
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return littleEndian ? qFromLittleEndian<quint16>(p) : qFromBigEndian<quint16>(p);
}

quint32 read32(const QByteArray &data, qsizetype offset, bool littleEndian)
{
    if (offset < 0 || offset + 4 > data.size()) {
        return 0;
    }
    const auto *p = reinterpret_cast<const uchar *>(data.constData() + offset);
    return littleEndian ? qFromLittleEndian<quint32>(p) : qFromBigEndian<quint32>(p);
}

void append16(QByteArray &data, quint16 value)
{
    const auto oldSize = data.size();
    data.resize(oldSize + 2);
    qToLittleEndian(value, reinterpret_cast<uchar *>(data.data() + oldSize));
}

void append32(QByteArray &data, quint32 value)
{
    const auto oldSize = data.size();
    data.resize(oldSize + 4);
    qToLittleEndian(value, reinterpret_cast<uchar *>(data.data() + oldSize));
}

qsizetype jpegHeaderInsertionPoint(const QByteArray &jpeg)
{
    if (jpeg.size() < 2 || uchar(jpeg[0]) != 0xff || uchar(jpeg[1]) != 0xd8) {
        return -1;
    }

    qsizetype offset = 2;
    while (offset + 4 <= jpeg.size() && uchar(jpeg[offset]) == 0xff
           && (uchar(jpeg[offset + 1]) == 0xe0 || uchar(jpeg[offset + 1]) == 0xe1)) {
        const quint16 length = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(jpeg.constData() + offset + 2));
        if (length < 2 || offset + 2 + length > jpeg.size()) {
            return -1;
        }
        offset += 2 + length;
    }
    return offset;
}

QByteArray jpegSegment(quint8 marker, const QByteArray &payload)
{
    if (payload.size() > 65533) {
        return {};
    }
    QByteArray result;
    result.reserve(payload.size() + 4);
    result.append(char(0xff));
    result.append(char(marker));
    const quint16 length = quint16(payload.size() + 2);
    result.append(char(length >> 8));
    result.append(char(length & 0xff));
    result.append(payload);
    return result;
}

QByteArray addJpegSegment(const QByteArray &jpeg, quint8 marker, const QByteArray &payload)
{
    const qsizetype insertionPoint = jpegHeaderInsertionPoint(jpeg);
    const QByteArray segment = jpegSegment(marker, payload);
    if (insertionPoint < 0 || segment.isEmpty()) {
        return {};
    }
    return jpeg.left(insertionPoint) + segment + jpeg.mid(insertionPoint);
}

QByteArray encodeJpeg(const QImage &image, int quality)
{
    QByteArray data;
    QBuffer buffer(&data);
    if (!buffer.open(QIODevice::WriteOnly)) {
        return {};
    }
    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality < 0 ? 100 : quality);
    if (!writer.write(image.convertToFormat(QImage::Format_RGB888))) {
        return {};
    }
    return data;
}

QByteArray jpsComment()
{
    return QByteArrayLiteral("Stereo layout: side-by-side; right eye first");
}

QByteArray makeMpfSegment(const QList<MpfEntry> &entries)
{
    QByteArray tiff;
    tiff.append("II", 2);
    append16(tiff, 42);
    append32(tiff, 8);

    const quint32 indexOffset = 8;
    const quint32 indexSize = 2 + 3 * 12 + 4;
    const quint32 attributeOffset = indexOffset + indexSize;
    const quint32 entryOffset = attributeOffset + indexSize;

    append16(tiff, 3);
    append16(tiff, 0xb000);
    append16(tiff, 7);
    append32(tiff, 4);
    tiff.append("0100", 4);
    append16(tiff, 0xb001);
    append16(tiff, 4);
    append32(tiff, 1);
    append32(tiff, entries.size());
    append16(tiff, 0xb002);
    append16(tiff, 7);
    append32(tiff, entries.size() * 16);
    append32(tiff, entryOffset);
    append32(tiff, attributeOffset);

    append16(tiff, 3);
    append16(tiff, 0xb000);
    append16(tiff, 7);
    append32(tiff, 4);
    tiff.append("0100", 4);
    append16(tiff, 0xb101);
    append16(tiff, 4);
    append32(tiff, 1);
    append32(tiff, 1);
    append16(tiff, 0xb204);
    append16(tiff, 4);
    append32(tiff, 1);
    append32(tiff, 1);
    append32(tiff, 0);

    for (const auto &entry : entries) {
        append32(tiff, entry.attributes);
        append32(tiff, entry.size);
        append32(tiff, entry.offset);
        append16(tiff, 0);
        append16(tiff, 0);
    }

    return jpegSegment(0xe2, QByteArrayLiteral("MPF\0") + tiff);
}

QByteArray makeMpo(const QImage &image, int quality)
{
    if (image.width() <= 1 || image.width() % 2 != 0) {
        return {};
    }

    const int eyeWidth = image.width() / 2;
    const QByteArray left = encodeJpeg(image.copy(0, 0, eyeWidth, image.height()), quality);
    QByteArray right = encodeJpeg(image.copy(eyeWidth, 0, eyeWidth, image.height()), quality);
    if (left.isEmpty() || right.isEmpty()) {
        return {};
    }
    right = addJpegSegment(right, 0xe2, makeMpfSegment({
                                             {0x020002, quint32(right.size()), 0},
                                         }));
    if (right.isEmpty()) {
        return {};
    }

    const qsizetype insertionPoint = jpegHeaderInsertionPoint(left);
    if (insertionPoint < 0) {
        return {};
    }
    const QByteArray placeholder = makeMpfSegment({
        {0x20020002, quint32(left.size()), 0},
        {0x020002, quint32(right.size()), 0},
    });
    const quint32 tiffOffset = quint32(insertionPoint + placeholder.indexOf("II"));
    const quint32 secondOffset = quint32(left.size() + placeholder.size() - tiffOffset);
    const QByteArray mpf = makeMpfSegment({
        {0x20020002, quint32(left.size() + placeholder.size()), 0},
        {0x020002, quint32(right.size()), secondOffset},
    });
    return left.left(insertionPoint) + mpf + left.mid(insertionPoint) + right;
}

std::optional<MpfData> parseMpf(const QByteArray &data)
{
    qsizetype offset = 2;
    while (offset + 4 <= data.size() && uchar(data[offset]) == 0xff) {
        const quint8 marker = uchar(data[offset + 1]);
        if (marker == 0xda || marker == 0xd9) {
            break;
        }
        if (marker == 0xe2) {
            const quint16 length = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(data.constData() + offset + 2));
            if (length >= 8 && offset + 2 + length <= data.size() && data.mid(offset + 4, 4) == QByteArrayLiteral("MPF\0")) {
                const qsizetype tiffOffset = offset + 8;
                const bool littleEndian = data.mid(tiffOffset, 2) == "II";
                if (!littleEndian && data.mid(tiffOffset, 2) != "MM") {
                    return std::nullopt;
                }
                const quint32 ifdOffset = read32(data, tiffOffset + 4, littleEndian);
                const qsizetype ifd = tiffOffset + ifdOffset;
                const quint16 count = read16(data, ifd, littleEndian);
                qsizetype entry = ifd + 2;
                quint32 numberOfImages = 0;
                quint32 mpEntryOffset = 0;
                for (quint16 i = 0; i < count; ++i, entry += 12) {
                    const quint16 tag = read16(data, entry, littleEndian);
                    const quint16 type = read16(data, entry + 2, littleEndian);
                    const quint32 value = read32(data, entry + 8, littleEndian);
                    if (tag == 0xb001 && type == 4) {
                        numberOfImages = value;
                    } else if (tag == 0xb002 && type == 7) {
                        mpEntryOffset = value;
                    }
                }
                if (numberOfImages != 2 || !mpEntryOffset) {
                    return std::nullopt;
                }
                QList<MpfEntry> entries;
                entry = tiffOffset + mpEntryOffset;
                for (quint32 i = 0; i < numberOfImages; ++i, entry += 16) {
                    entries.append({read32(data, entry, littleEndian), read32(data, entry + 4, littleEndian), read32(data, entry + 8, littleEndian)});
                }
                return MpfData{tiffOffset, entries};
            }
        }
        if (marker == 0xd8 || marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7)) {
            offset += 2;
        } else {
            const quint16 length = qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(data.constData() + offset + 2));
            if (length < 2 || offset + 2 + length > data.size()) {
                break;
            }
            offset += 2 + length;
        }
    }
    return std::nullopt;
}

QList<QByteArray> splitMpo(const QByteArray &data)
{
    const auto mpf = parseMpf(data);
    if (!mpf || mpf->entries.size() != 2) {
        return {};
    }
    QList<QByteArray> result;
    for (const auto &entry : mpf->entries) {
        const qsizetype start = entry.offset == 0 ? 0 : mpf->tiffOffset + entry.offset;
        if (start < 0 || entry.size == 0 || start + entry.size > data.size()) {
            return {};
        }
        result.append(data.mid(start, entry.size));
    }
    return result;
}

} // namespace

class StereoHandler final : public QImageIOHandler
{
public:
    bool canRead() const override
    {
        if (!device()) {
            return false;
        }
        if (isFormat(format(), jpsFormat) || isFormat(format(), mpoFormat)) {
            return device()->peek(2) == QByteArrayLiteral("\xff\xd8");
        }
        return false;
    }

    bool read(QImage *image) override
    {
        if (!scan()) {
            return false;
        }
        if (m_images.isEmpty() || m_currentImage >= m_images.size()) {
            return false;
        }
        *image = QImage::fromData(m_images.at(m_currentImage), "jpeg");
        if (image->isNull()) {
            return false;
        }
        image->setText(QString::fromLatin1(stereoLayoutKey), m_isMpo ? QString::fromLatin1(mpoLayout) : QString::fromLatin1(jpsLayout));
        return true;
    }

    bool write(const QImage &image) override
    {
        const int quality = m_quality;
        QByteArray data;
        if (isFormat(format(), mpoFormat)) {
            data = makeMpo(image, quality);
        } else {
            if (image.width() <= 1 || image.width() % 2 != 0) {
                return false;
            }
            const int width = image.width() / 2;
            QImage jps(image.size(), QImage::Format_RGB888);
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < width; ++x) {
                    jps.setPixelColor(x, y, image.pixelColor(width + x, y));
                    jps.setPixelColor(width + x, y, image.pixelColor(x, y));
                }
            }
            data = encodeJpeg(jps, quality);
            data = addJpegSegment(data, 0xfe, jpsComment());
        }
        if (data.isEmpty()) {
            return false;
        }
        return device()->write(data) == data.size();
    }

    int currentImageNumber() const override
    {
        return m_currentImage;
    }

    int imageCount() const override
    {
        if (!scan()) {
            return 0;
        }
        return m_images.size();
    }

    bool jumpToImage(int imageNumber) override
    {
        if (!scan() || imageNumber < 0 || imageNumber >= m_images.size()) {
            return false;
        }
        m_currentImage = imageNumber;
        return true;
    }

    bool jumpToNextImage() override
    {
        return jumpToImage(m_currentImage + 1);
    }

    bool supportsOption(ImageOption option) const override
    {
        return option == Size || option == ImageFormat || option == Quality;
    }

    QVariant option(ImageOption option) const override
    {
        if (option == Size && scan() && !m_images.isEmpty()) {
            return QImage::fromData(m_images.first(), "jpeg").size();
        }
        if (option == ImageFormat) {
            return QByteArray("jpeg");
        }
        if (option == Quality) {
            return m_quality;
        }
        return {};
    }

    void setOption(ImageOption option, const QVariant &value) override
    {
        if (option == Quality) {
            m_quality = value.toInt();
        }
    }

private:
    bool scan() const
    {
        if (m_scanned) {
            return !m_images.isEmpty();
        }
        m_scanned = true;
        if (!device()->isSequential()) {
            device()->seek(0);
        }
        const QByteArray data = device()->readAll();
        if (isFormat(format(), mpoFormat)) {
            m_isMpo = true;
            m_images = splitMpo(data);
        } else if (data.startsWith("\xff\xd8")) {
            m_images.append(data);
        }
        return !m_images.isEmpty();
    }

    mutable bool m_scanned = false;
    mutable QList<QByteArray> m_images;
    mutable bool m_isMpo = false;
    int m_currentImage = 0;
    int m_quality = -1;
};

class StereoPlugin final : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "stereo.json")

public:
    Capabilities capabilities(QIODevice *, const QByteArray &format) const override
    {
        if (isFormat(format, jpsFormat) || isFormat(format, mpoFormat)) {
            return Capabilities(CanRead | CanWrite);
        }
        return {};
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format = {}) const override
    {
        auto *handler = new StereoHandler;
        handler->setDevice(device);
        handler->setFormat(format);
        return handler;
    }
};

#include "stereo.moc"
