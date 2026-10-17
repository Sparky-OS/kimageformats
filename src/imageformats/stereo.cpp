/*
    SPDX-FileCopyrightText: 2026 The KDE Contributors
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-or-later OR LGPL-3.0-or-later OR LicenseRef-KDE-Accepted-LGPL
*/

#include "depthmark_p.h"

#include <QBuffer>
#include <QImage>
#include <QImageIOPlugin>
#include <QImageReader>
#include <QImageWriter>
#include <QList>
#include <QVariant>
#include <QtEndian>

#include <functional>
#include <optional>

namespace
{
constexpr QByteArrayView jpsFormat = "jps";
constexpr QByteArrayView mpoFormat = "mpo";
// a JPEG picture with its depth map (Dynamic Depth, GDepth, Apple's depth data)
constexpr QByteArrayView depthFormat = "jpeg";
constexpr QByteArrayView stereoLayoutKey = "StereoLayout";
constexpr QByteArrayView stereo3DLayoutKey = "Stereo3DLayout";
constexpr QByteArrayView jpsLayout = "side-by-side; right eye first";
constexpr QByteArrayView mpoLayout = "multi-picture object; left eye first";

QString legacyLayout(const QString &layout)
{
    if (layout.startsWith(QStringLiteral("sideBySide"))) {
        return layout.endsWith(QStringLiteral("LeftFirst")) ? QStringLiteral("side-by-side; left eye first") : QString::fromLatin1(jpsLayout);
    }
    if (layout.startsWith(QStringLiteral("topBottom"))) {
        return layout.endsWith(QStringLiteral("LeftFirst")) ? QStringLiteral("top-and-bottom; left eye first")
                                                            : QStringLiteral("top-and-bottom; right eye first");
    }
    if (layout.startsWith(QStringLiteral("multiView"))) {
        return layout.endsWith(QStringLiteral("LeftFirst")) ? QString::fromLatin1(mpoLayout) : QStringLiteral("multi-picture object; right eye first");
    }
    return {};
}

bool isFormat(const QByteArray &format, QByteArrayView expected)
{
    return format.compare(expected, Qt::CaseInsensitive) == 0;
}

struct MpfEntry {
    quint32 attributes = 0;
    quint32 size = 0;
    quint32 offset = 0;
};

struct JpegHeader {
    bool valid = false;
    bool jpsPresent = false;
    QByteArray jps;
    QByteArray mpf;
    qsizetype tiffOffset = 0;
    // XMP Specification Part 3, 1.1.3.1: the main packet and the extension's chunks
    QByteArray xmp;
    QList<QByteArray> extendedXmp;
};

constexpr QByteArrayView xmpSignature("http://ns.adobe.com/xap/1.0/\0", 29);
constexpr QByteArrayView extendedXmpSignature("http://ns.adobe.com/xmp/extension/\0", 35);

JpegHeader readJpegHeader(const QByteArray &data, qsizetype limit = 1024 * 1024)
{
    JpegHeader result;
    if (!data.startsWith(QByteArrayLiteral("\xff\xd8"))) {
        return result;
    }
    qsizetype offset = 2;
    while (offset < data.size() && offset < limit) {
        if (quint8(data.at(offset++)) != 0xff) {
            return {};
        }
        while (offset < data.size() && quint8(data.at(offset)) == 0xff) {
            ++offset;
        }
        if (offset >= data.size()) {
            return {};
        }
        const quint8 marker = quint8(data.at(offset++));
        if (marker == 0 || marker == 0xd8 || marker == 0xd9) {
            return {};
        }
        if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7)) {
            continue;
        }
        if (offset > data.size() - 2) {
            return {};
        }
        const quint16 length = qFromBigEndian<quint16>(data.constData() + offset);
        if (length < 2 || length > data.size() - offset) {
            return {};
        }
        if (marker == 0xda) {
            result.valid = length >= 6;
            return result;
        }
        const QByteArray payload = data.mid(offset + 2, length - 2);
        if (marker == 0xe2 && payload.startsWith(QByteArrayLiteral("MPF\0"))) {
            if (!result.mpf.isEmpty()) {
                return {};
            }
            result.mpf = payload.mid(4);
            result.tiffOffset = offset + 6;
        } else if (marker == 0xe1 && payload.startsWith(xmpSignature)) {
            result.xmp = payload.mid(xmpSignature.size());
        } else if (marker == 0xe1 && payload.startsWith(extendedXmpSignature)) {
            result.extendedXmp.append(payload.mid(extendedXmpSignature.size()));
        } else if (marker == 0xe3 && payload.startsWith("_JPSJPS_")) {
            if (result.jpsPresent) {
                return {};
            }
            result.jpsPresent = true;
            result.jps = payload.mid(8);
        }
        offset += length;
    }
    return {};
}

QString jpsStereoLayout(const QByteArray &data)
{
    if (data.size() < 6) {
        return {};
    }
    const quint16 length = qFromBigEndian<quint16>(data.constData());
    if (length < 4 || length > data.size() - 2) {
        return {};
    }
    const quint32 descriptor = qFromBigEndian<quint32>(data.constData() + 2);
    const quint8 type = descriptor & 0xff;
    if (type == 0) {
        return QStringLiteral("mono");
    }
    if (type != 1) {
        return {};
    }
    QString layout;
    switch ((descriptor >> 8) & 0xff) {
    case 1:
        layout = QStringLiteral("rowInterleaved");
        break;
    case 2:
        layout = QStringLiteral("sideBySide");
        break;
    case 3:
        layout = QStringLiteral("topBottom");
        break;
    case 4:
        return (descriptor & 0x40000) ? QStringLiteral("anaglyphRedLeft") : QStringLiteral("anaglyphRedRight");
    default:
        return {};
    }
    return layout + ((descriptor & 0x40000) ? QStringLiteral("LeftFirst") : QStringLiteral("RightFirst"));
}

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
    while (offset + 4 <= jpeg.size() && uchar(jpeg[offset]) == 0xff && (uchar(jpeg[offset + 1]) == 0xe0 || uchar(jpeg[offset + 1]) == 0xe1)) {
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
    QByteArray attributes("II", 2);
    append16(attributes, 42);
    append32(attributes, 8);
    append16(attributes, 3);
    append16(attributes, 0xb000);
    append16(attributes, 7);
    append32(attributes, 4);
    attributes.append("0100", 4);
    append16(attributes, 0xb101);
    append16(attributes, 4);
    append32(attributes, 1);
    append32(attributes, 2);
    append16(attributes, 0xb204);
    append16(attributes, 4);
    append32(attributes, 1);
    append32(attributes, 1);
    append32(attributes, 0);
    right = addJpegSegment(right, 0xe2, QByteArrayLiteral("MPF\0") + attributes);
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

class MpfReader
{
public:
    explicit MpfReader(const QByteArray &data)
        : m_data(data)
        , m_little(data.startsWith("II"))
    {
    }

    bool valid() const
    {
        return m_data.size() >= 8 && (m_data.startsWith("II") || m_data.startsWith("MM")) && read16(m_data, 2, m_little) == 42;
    }

    quint32 value(qsizetype pos) const
    {
        return read32(m_data, pos, m_little);
    }

    qsizetype field(quint32 ifd, quint16 tag, quint16 type, quint32 count) const
    {
        if (ifd < 8 || ifd > quint32(m_data.size() - 2)) {
            return -1;
        }
        const quint16 fields = read16(m_data, ifd, m_little);
        if (qint64(ifd) + 2 + qint64(fields) * 12 + 4 > m_data.size()) {
            return -1;
        }
        for (qsizetype pos = ifd + 2; pos < ifd + 2 + fields * 12; pos += 12) {
            if (read16(m_data, pos, m_little) == tag) {
                return read16(m_data, pos + 2, m_little) == type && value(pos + 4) == count ? pos + 8 : -1;
            }
        }
        return -1;
    }

    quint32 nextIfd(quint32 ifd) const
    {
        if (ifd < 8 || ifd > quint32(m_data.size() - 2)) {
            return 0;
        }
        return value(qsizetype(ifd) + 2 + qsizetype(read16(m_data, ifd, m_little)) * 12);
    }

    quint32 viewpoint(quint32 ifd) const
    {
        const qsizetype pos = field(ifd, 0xb101, 4, 1);
        return pos < 0 ? 0 : value(pos);
    }

private:
    const QByteArray &m_data;
    bool m_little;
};

std::optional<MpfData> parseMpf(const QByteArray &data)
{
    const JpegHeader header = readJpegHeader(data);
    const MpfReader index(header.mpf);
    if (!header.valid || !index.valid()) {
        return std::nullopt;
    }
    const quint32 ifd = index.value(4);
    const qsizetype number = index.field(ifd, 0xb001, 4, 1);
    const qsizetype entries = index.field(ifd, 0xb002, 7, 32);
    if (number < 0 || index.value(number) != 2 || entries < 0) {
        return std::nullopt;
    }
    const quint32 offset = index.value(entries);
    if (qint64(offset) + 32 > header.mpf.size()) {
        return std::nullopt;
    }
    QList<MpfEntry> records;
    for (int i = 0; i < 2; ++i) {
        const qsizetype pos = offset + i * 16;
        records.append({index.value(pos), index.value(pos + 4), index.value(pos + 8)});
    }
    return MpfData{header.tiffOffset, records};
}

// Whether the Multi-Picture Format index describes a stereo pair: two images, both of MP Type "Multi-frame Image:
// Disparity Image" (CIPA DC-007 5.2.3.3.1, 020002.H). Other two-image files, such as a picture and its HDR gain map,
// are ordinary JPEG pictures.
bool isStereoMpf(const QByteArray &data)
{
    const auto mpf = parseMpf(data);
    if (!mpf) {
        return false;
    }
    for (const MpfEntry &entry : mpf->entries) {
        if ((entry.attributes & 0x07ffffff) != 0x020002) {
            return false;
        }
    }
    return true;
}

QString mpoStereoLayout(const QByteArray &data)
{
    const auto mpf = parseMpf(data);
    if (!mpf) {
        return {};
    }
    for (int i = 0; i < 2; ++i) {
        const MpfEntry &entry = mpf->entries.at(i);
        const qint64 start = i == 0 ? 0 : qint64(mpf->tiffOffset) + entry.offset;
        if ((entry.attributes & 0x07ffffff) != 0x020002 || (i == 0 && entry.offset != 0) || entry.size < 4 || start > data.size() - qint64(entry.size)
            || data.mid(start + entry.size - 2, 2) != QByteArrayLiteral("\xff\xd9")) {
            return {};
        }
    }
    const MpfEntry &secondEntry = mpf->entries.at(1);
    const qsizetype secondStart = mpf->tiffOffset + secondEntry.offset;
    if (secondStart < mpf->entries.at(0).size) {
        return {};
    }
    const JpegHeader firstHeader = readJpegHeader(data);
    const MpfReader first(firstHeader.mpf);
    const JpegHeader secondHeader = readJpegHeader(data.mid(secondStart, secondEntry.size));
    const MpfReader second(secondHeader.mpf);
    if (!secondHeader.valid || !second.valid()) {
        return {};
    }
    const quint32 firstView = first.viewpoint(first.nextIfd(first.value(4)));
    const quint32 secondView = second.viewpoint(second.value(4));
    if (firstView == 0 || secondView == 0 || firstView == 0xffffffff || secondView == 0xffffffff || firstView == secondView) {
        return {};
    }
    return firstView < secondView ? QStringLiteral("multiViewLeftFirst") : QStringLiteral("multiViewRightFirst");
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

// Where the primary image of a JPEG file ends: after its EOI marker, the entropy-coded data of each scan skipped.
qsizetype jpegEnd(const QByteArray &data)
{
    qsizetype offset = 2;
    while (offset + 2 <= data.size()) {
        if (quint8(data.at(offset)) != 0xff) {
            return -1;
        }
        const quint8 marker = quint8(data.at(offset + 1));
        if (marker == 0xff) {
            ++offset;
            continue;
        }
        if (marker == 0xd9) {
            return offset + 2;
        }
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
            offset += 2;
            continue;
        }
        if (offset + 4 > data.size()) {
            return -1;
        }
        offset += 2 + qFromBigEndian<quint16>(data.constData() + offset + 2);
        if (marker == 0xda) {
            while (offset + 1 < data.size()) {
                if (quint8(data.at(offset)) != 0xff) {
                    ++offset;
                    continue;
                }
                const quint8 next = quint8(data.at(offset + 1));
                if (next == 0 || (next >= 0xd0 && next <= 0xd7)) {
                    offset += 2;
                } else if (next == 0xff) {
                    ++offset;
                } else {
                    break;
                }
            }
        }
    }
    return -1;
}

// Every image of a CIPA DC-007 Multi-Picture Format index, the first being the file's primary image.
QList<MpfEntry> mpfEntries(const JpegHeader &header)
{
    const MpfReader index(header.mpf);
    if (!header.valid || !index.valid()) {
        return {};
    }
    const quint32 ifd = index.value(4);
    const qsizetype number = index.field(ifd, 0xb001, 4, 1);
    const quint32 count = number < 0 ? 0 : index.value(number);
    if (count < 2 || count > 64) {
        return {};
    }
    const qsizetype entries = index.field(ifd, 0xb002, 7, count * 16);
    if (entries < 0) {
        return {};
    }
    const quint32 offset = index.value(entries);
    if (qint64(offset) + count * 16 > header.mpf.size()) {
        return {};
    }
    QList<MpfEntry> records;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype pos = offset + i * 16;
        records.append({index.value(pos), index.value(pos + 4), index.value(pos + 8)});
    }
    return records;
}

DepthMark::Xmp jpegXmp(const JpegHeader &header)
{
    DepthMark::Xmp xmp;
    xmp.add(header.xmp);
    const QString guid = xmp.value(u"http://ns.adobe.com/xmp/note/", u"HasExtendedXMP");
    if (!guid.isEmpty()) {
        xmp.add(DepthMark::extendedXmp(header.extendedXmp, guid.toLatin1()));
    }
    return xmp;
}

QByteArray formatOfMime(const QString &mime)
{
    if (mime == QLatin1String("image/jpeg")) {
        return QByteArrayLiteral("jpeg");
    }
    if (mime == QLatin1String("image/png")) {
        return QByteArrayLiteral("png");
    }
    if (mime == QLatin1String("image/tiff")) {
        return QByteArrayLiteral("tiff");
    }
    if (mime == QLatin1String("image/heif") || mime == QLatin1String("image/heic")) {
        return QByteArrayLiteral("heif");
    }
    return {};
}

bool startsLike(const QByteArray &data, const QByteArray &format)
{
    if (format == "jpeg") {
        return data.startsWith("\xff\xd8");
    }
    if (format == "png") {
        return data.startsWith("\x89PNG");
    }
    if (format == "tiff") {
        return data.startsWith(QByteArrayView("II*\0", 4)) || data.startsWith(QByteArrayView("MM\0*", 4));
    }
    if (format == "heif") {
        return data.mid(4, 4) == "ftyp";
    }
    return false;
}

// A picture with its depth map in one JPEG file (Sparky Stereo OS's depth marks, depthSeparate): the encoded depth map,
// the picture before the camera's effects when the file has it, and the Stereo3DDepth mark.
struct JpegDepth {
    QString mark;
    QByteArray depth;
    QByteArray depthFormat;
    QByteArray original;
    QByteArray originalFormat;
};

// Reads count bytes at a position of the file data comes from: data itself when it holds them.
using ReadAt = std::function<QByteArray(qint64, qint64)>;

std::optional<JpegDepth> dynamicDepth(const QByteArray &data, const DepthMark::Xmp &xmp, bool whole)
{
    // Dynamic Depth 1.0: the Device's Cameras, the DepthMap of the Depth Photo profile's camera or of the first camera
    // with one ("Depth images for the first camera must have no holes"), its Container item by DepthURI.
    if (xmp.items.isEmpty()) {
        return std::nullopt;
    }
    int camera = -1;
    for (const int index : xmp.depthPhotoCameras) {
        if (index >= 0 && index < xmp.depthMaps.size() && xmp.depthMaps.at(index).valid) {
            camera = index;
            break;
        }
    }
    for (int i = 0; camera < 0 && i < xmp.depthMaps.size(); ++i) {
        if (xmp.depthMaps.at(i).valid) {
            camera = i;
        }
    }
    if (camera < 0) {
        return std::nullopt;
    }
    const auto &map = xmp.depthMaps.at(camera);
    // ItemSemantic Segmentation: a segmentation mask, not depth
    if (!map.semantic.isEmpty() && map.semantic != QLatin1String("Depth")) {
        return std::nullopt;
    }
    JpegDepth result;
    result.mark = DepthMark::fromRange(map.format, map.near, map.far);
    if (result.mark.isEmpty() || !whole) {
        return result.mark.isEmpty() ? std::nullopt : std::optional(result);
    }

    // "The location of media item resources in the container is determined by summing the Length's of the preceding
    // secondary item resources to the length of the primary image encoding plus Padding if specified."
    const qsizetype primaryEnd = jpegEnd(data);
    const auto locate = [&xmp](qint64 first) {
        QList<QPair<qint64, qint64>> places(xmp.items.size(), {0, 0});
        qint64 pos = first;
        for (int i = 1; i < xmp.items.size(); ++i) {
            const qint64 length = xmp.items.at(i).length;
            places[i] = length == 0 ? places.at(i - 1) : QPair<qint64, qint64>(pos, length);
            pos += length;
        }
        return places;
    };
    const auto item = [&](const QString &uri, QByteArray *bytes, QByteArray *format) {
        qint64 total = 0;
        for (int i = 1; i < xmp.items.size(); ++i) {
            total += xmp.items.at(i).length;
        }
        for (int i = 1; i < xmp.items.size(); ++i) {
            if (xmp.items.at(i).dataUri != uri) {
                continue;
            }
            *format = formatOfMime(xmp.items.at(i).mime);
            // the items packed after the primary image; else, for a file with more after its primary image than the
            // directory lists, the items packed at the end
            for (const qint64 first : {primaryEnd < 0 ? qint64(-1) : primaryEnd + xmp.items.first().padding, data.size() - total}) {
                const auto place = locate(first).at(i);
                if (first >= 0 && place.first + place.second <= data.size() && startsLike(data.mid(place.first, 16), *format)) {
                    *bytes = data.mid(place.first, place.second);
                    return true;
                }
            }
        }
        return false;
    };
    if (!item(map.depthUri, &result.depth, &result.depthFormat)) {
        return std::nullopt;
    }
    if (camera < xmp.images.size() && xmp.images.at(camera).first == QLatin1String("Original")) {
        item(xmp.images.at(camera).second, &result.original, &result.originalFormat);
    }
    return result;
}

std::optional<JpegDepth> googleDepth(const DepthMark::Xmp &xmp, bool whole)
{
    // GDepth (Google's depthmap metadata): Format, Near, Far, Mime and Data, the depth map's file in base64; GImage,
    // the image before Google Camera's Lens Blur.
    static const QString gdepth = QStringLiteral("http://ns.google.com/photos/1.0/depthmap/");
    static const QString gimage = QStringLiteral("http://ns.google.com/photos/1.0/image/");
    JpegDepth result;
    result.mark = DepthMark::fromRange(xmp.value(gdepth, u"Format"), xmp.value(gdepth, u"Near").toDouble(), xmp.value(gdepth, u"Far").toDouble());
    if (result.mark.isEmpty() || !whole) {
        return result.mark.isEmpty() ? std::nullopt : std::optional(result);
    }
    result.depth = QByteArray::fromBase64(xmp.value(gdepth, u"Data").toLatin1());
    result.depthFormat = formatOfMime(xmp.value(gdepth, u"Mime"));
    if (!startsLike(result.depth, result.depthFormat)) {
        return std::nullopt;
    }
    result.original = QByteArray::fromBase64(xmp.value(gimage, u"Data").toLatin1());
    result.originalFormat = formatOfMime(xmp.value(gimage, u"Mime"));
    if (!startsLike(result.original, result.originalFormat)) {
        result.original.clear();
    }
    return result;
}

std::optional<JpegDepth> appleDepth(const JpegHeader &header, const ReadAt &readAt, bool whole)
{
    // Apple's portrait JPEG: Multi-Picture Format images after the primary one, each with its depth data description
    // (AuxiliaryImageType, NativeFormat) in its own XMP; the disparity or depth map is the one that describes depth.
    const QList<MpfEntry> entries = mpfEntries(header);
    for (int i = 1; i < entries.size(); ++i) {
        const qint64 start = header.tiffOffset + qint64(entries.at(i).offset);
        const QByteArray image = readAt(start, whole ? entries.at(i).size : qMin<qint64>(entries.at(i).size, 64 * 1024));
        const JpegHeader imageHeader = readJpegHeader(image);
        if (!imageHeader.valid) {
            continue;
        }
        const DepthMark::Xmp xmp = jpegXmp(imageHeader);
        if (!DepthMark::isAppleDepth(xmp)) {
            continue;
        }
        JpegDepth result;
        result.mark = DepthMark::fromAppleDepthData(xmp, 8);
        if (result.mark.isEmpty()) {
            continue;
        }
        if (whole) {
            result.depth = image;
            result.depthFormat = QByteArrayLiteral("jpeg");
        }
        return result;
    }
    return std::nullopt;
}

// The depth map of a JPEG file, data being the whole file (whole) or its beginning.
std::optional<JpegDepth> jpegDepth(const QByteArray &data, const ReadAt &readAt, bool whole)
{
    const JpegHeader header = readJpegHeader(data, whole ? data.size() : 1024 * 1024);
    if (!header.valid) {
        return std::nullopt;
    }
    const DepthMark::Xmp xmp = jpegXmp(header);
    if (auto depth = dynamicDepth(data, xmp, whole)) {
        return depth;
    }
    if (auto depth = googleDepth(xmp, whole)) {
        return depth;
    }
    return appleDepth(header, readAt, whole);
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
        if (isFormat(format(), jpsFormat) || isFormat(format(), mpoFormat) || isFormat(format(), depthFormat)) {
            // once scanned, the device has been read to its end
            return m_scanned ? !m_images.isEmpty() : device()->peek(2) == QByteArrayLiteral("\xff\xd8");
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
        *image = QImage::fromData(m_images.at(m_currentImage), m_formats.value(m_currentImage, QByteArrayLiteral("jpeg")).constData());
        if (image->isNull()) {
            return false;
        }
        if (!m_depthMark.isEmpty() && m_currentImage == 1) {
            *image = DepthMark::grayDepth(*image);
        }
        const QString legacy = legacyLayout(m_stereo3DLayout);
        if (!legacy.isEmpty()) {
            image->setText(QString::fromLatin1(stereoLayoutKey), legacy);
        }
        if (!m_stereo3DLayout.isEmpty()) {
            image->setText(QString::fromLatin1(stereo3DLayoutKey), m_stereo3DLayout);
        }
        if (!m_depthMark.isEmpty()) {
            image->setText(DepthMark::depthKey, m_depthMark);
        }
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
        return option == Size || option == ImageFormat || option == Quality || option == Description
            || (option == ImageTransformation && isFormat(format(), depthFormat));
    }

    QVariant option(ImageOption option) const override
    {
        if (option == Description && scan()) {
            return DepthMark::description(m_stereo3DLayout, m_depthMark);
        }
        if (option == Size && scan() && !m_images.isEmpty()) {
            return imageSize(m_currentImage);
        }
        if (option == ImageTransformation && scan()) {
            // A depth map without an orientation of its own is registered with the picture as stored: it takes the
            // picture's (Android's Dynamic Depth writer gives its depth JPEG the picture's EXIF orientation).
            const Transformations current = imageTransformation(m_currentImage);
            return int(current == TransformationNone && m_currentImage > 0 ? imageTransformation(0) : current);
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
        if (isFormat(format(), depthFormat)) {
            const auto depth = jpegDepth(
                data,
                [&data](qint64 pos, qint64 count) {
                    return data.mid(pos, count);
                },
                true);
            m_images.append(data);
            m_formats.append(QByteArrayLiteral("jpeg"));
            if (depth) {
                m_images.append(depth->depth);
                m_formats.append(depth->depthFormat);
                if (!depth->original.isEmpty()) {
                    m_images.append(depth->original);
                    m_formats.append(depth->originalFormat);
                }
                m_stereo3DLayout = DepthMark::separate;
                m_depthMark = depth->mark;
            }
        } else if (isFormat(format(), mpoFormat)) {
            m_images = splitMpo(data);
            m_stereo3DLayout = mpoStereoLayout(data);
        } else if (data.startsWith("\xff\xd8")) {
            m_images.append(data);
            const JpegHeader header = readJpegHeader(data);
            if (header.valid) {
                m_stereo3DLayout = header.jpsPresent ? jpsStereoLayout(header.jps) : QStringLiteral("sideBySideRightFirst");
            }
        }
        return !m_images.isEmpty();
    }

    // The size and the transformation of an image, from its header.
    QSize imageSize(int index) const
    {
        QByteArray data = m_images.value(index);
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadOnly);
        return QImageReader(&buffer, m_formats.value(index, QByteArrayLiteral("jpeg"))).size();
    }

    Transformations imageTransformation(int index) const
    {
        QByteArray data = m_images.value(index);
        QBuffer buffer(&data);
        buffer.open(QIODevice::ReadOnly);
        return QImageReader(&buffer, m_formats.value(index, QByteArrayLiteral("jpeg"))).transformation();
    }

    mutable bool m_scanned = false;
    mutable QList<QByteArray> m_images;
    mutable QList<QByteArray> m_formats;
    mutable QString m_stereo3DLayout;
    mutable QString m_depthMark;
    int m_currentImage = 0;
    int m_quality = -1;
};

class StereoPlugin final : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "stereo.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override
    {
        if (isFormat(format, jpsFormat) || isFormat(format, mpoFormat)) {
            return Capabilities(CanRead | CanWrite);
        }
        if (format.isEmpty() && device && device->isReadable()) {
            const QByteArray data = device->peek(1024 * 1024);
            const JpegHeader header = readJpegHeader(data);
            if (header.valid && (header.jpsPresent || isStereoMpf(data) || jpegDepth(data, reader(device, data), false))) {
                return CanRead;
            }
        }
        return {};
    }

    QImageIOHandler *create(QIODevice *device, const QByteArray &format = {}) const override
    {
        auto *handler = new StereoHandler;
        handler->setDevice(device);
        if (isFormat(format, jpsFormat) || isFormat(format, mpoFormat)) {
            handler->setFormat(format);
        } else {
            const QByteArray data = device->peek(1024 * 1024);
            const JpegHeader header = readJpegHeader(data);
            if (isStereoMpf(data)) {
                handler->setFormat(QByteArray(mpoFormat.data(), mpoFormat.size()));
            } else if (!header.jpsPresent && jpegDepth(data, reader(device, data), false)) {
                handler->setFormat(QByteArray(depthFormat.data(), depthFormat.size()));
            } else {
                handler->setFormat(QByteArray(jpsFormat.data(), jpsFormat.size()));
            }
        }
        return handler;
    }

private:
    // Bytes of the file at any position, for the images that a Multi-Picture Format index places after the first
    // megabyte: device is moved to them and back.
    static ReadAt reader(QIODevice *device, const QByteArray &peeked)
    {
        return [device, peeked](qint64 pos, qint64 count) {
            if (pos < 0 || count <= 0) {
                return QByteArray();
            }
            if (pos + count <= peeked.size() || device->isSequential()) {
                return peeked.mid(pos, count);
            }
            const qint64 at = device->pos();
            QByteArray bytes;
            if (device->seek(pos)) {
                bytes = device->read(count);
            }
            device->seek(at);
            return bytes;
        };
    }
};

#include "stereo.moc"
