/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-or-later OR LGPL-3.0-or-later OR LicenseRef-KDE-Accepted-LGPL
*/

#include "depthmark_p.h"

#include <QLocale>
#include <QXmlStreamReader>
#include <QtEndian>

#include <cmath>
#include <optional>

namespace
{
const QString deviceNs = QStringLiteral("http://ns.google.com/photos/dd/1.0/device/");
const QString containerNs = QStringLiteral("http://ns.google.com/photos/dd/1.0/container/");
const QString itemNs = QStringLiteral("http://ns.google.com/photos/dd/1.0/item/");
const QString cameraNs = QStringLiteral("http://ns.google.com/photos/dd/1.0/camera/");
const QString depthMapNs = QStringLiteral("http://ns.google.com/photos/dd/1.0/depthmap/");
const QString imageNs = QStringLiteral("http://ns.google.com/photos/dd/1.0/image/");
const QString profileNs = QStringLiteral("http://ns.google.com/photos/dd/1.0/profile/");
const QString appleNs = QStringLiteral("http://ns.apple.com/pixeldatainfo/1.0/");

// The bits of an RBSP: a NAL unit's payload with its emulation prevention bytes taken out.
class BitReader
{
public:
    explicit BitReader(const QByteArray &data)
        : m_data(data)
    {
    }

    bool atEnd() const
    {
        return m_failed || m_pos >= qint64(m_data.size()) * 8;
    }

    bool failed() const
    {
        return m_failed;
    }

    quint32 u(int bits)
    {
        quint32 value = 0;
        for (int i = 0; i < bits; ++i) {
            if (m_pos >= qint64(m_data.size()) * 8) {
                m_failed = true;
                return 0;
            }
            value = (value << 1) | ((quint8(m_data.at(m_pos >> 3)) >> (7 - (m_pos & 7))) & 1);
            ++m_pos;
        }
        return value;
    }

    quint32 ue()
    {
        int zeros = 0;
        while (u(1) == 0) {
            if (m_failed || ++zeros > 31) {
                m_failed = true;
                return 0;
            }
        }
        return zeros ? (quint32(1) << zeros) - 1 + u(zeros) : 0;
    }

private:
    const QByteArray m_data;
    qint64 m_pos = 0;
    bool m_failed = false;
};

QByteArray unescape(const QByteArray &nal)
{
    QByteArray rbsp;
    rbsp.reserve(nal.size());
    int zeros = 0;
    for (const char c : nal) {
        if (zeros >= 2 && quint8(c) == 3) {
            zeros = 0;
            continue;
        }
        zeros = c == 0 ? zeros + 1 : 0;
        rbsp.append(c);
    }
    return rbsp;
}

// depth_rep_info_element() (H.265 G.14.2.4.2) and depth_representation_sei_element() (H.264): x of Table G.4.
std::optional<double> element(BitReader &bits)
{
    const int sign = bits.u(1);
    const int exponent = bits.u(7);
    const int length = bits.u(5) + 1;
    const double mantissa = bits.u(length);
    if (bits.failed() || exponent == 127) {
        return std::nullopt;
    }
    const double magnitude = exponent > 0 ? std::ldexp(1.0 + mantissa / std::ldexp(1.0, length), exponent - 31) : std::ldexp(mantissa, -(30 + length));
    return sign ? -magnitude : magnitude;
}

quint32 be32(const QByteArray &data, qsizetype pos)
{
    return pos >= 0 && pos + 4 <= data.size() ? qFromBigEndian<quint32>(data.constData() + pos) : 0;
}

quint16 be16(const QByteArray &data, qsizetype pos)
{
    return pos >= 0 && pos + 2 <= data.size() ? qFromBigEndian<quint16>(data.constData() + pos) : 0;
}

// An ISO/IEC 14496-12 box: its type and where its payload starts and ends.
struct Box {
    QByteArray type;
    qsizetype payload = 0;
    qsizetype end = 0;
};

QList<Box> boxes(const QByteArray &data, qsizetype begin, qsizetype end)
{
    QList<Box> result;
    qsizetype pos = begin;
    while (pos + 8 <= end) {
        quint64 size = be32(data, pos);
        qsizetype header = 8;
        if (size == 1) {
            if (pos + 16 > end) {
                break;
            }
            size = (quint64(be32(data, pos + 8)) << 32) | be32(data, pos + 12);
            header = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < quint64(header) || size > quint64(end - pos)) {
            // A box running past the data read: kept as far as it goes, for the MetaBox at the start of a long file.
            result.append({data.mid(pos + 4, 4), pos + header, end});
            break;
        }
        result.append({data.mid(pos + 4, 4), pos + header, qsizetype(pos + size)});
        pos += size;
    }
    return result;
}

std::optional<Box> child(const QByteArray &data, const Box &parent, const char *type, qsizetype skip = 0)
{
    for (const Box &box : boxes(data, parent.payload + skip, parent.end)) {
        if (box.type == type) {
            return box;
        }
    }
    return std::nullopt;
}

QString fourcc(const QString &value)
{
    bool numeric = false;
    const quint32 code = value.toUInt(&numeric);
    if (!numeric) {
        return value;
    }
    QString text;
    for (int shift = 24; shift >= 0; shift -= 8) {
        text.append(QLatin1Char(char((code >> shift) & 0xff)));
    }
    return text;
}
}

namespace DepthMark
{
QString number(double value)
{
    return QString::number(value, 'g', QLocale::FloatingPointShortest);
}

QString description(const QString &layout, const QString &depth)
{
    QString text;
    if (!layout.isEmpty()) {
        text = QString(layoutKey) + QStringLiteral(": ") + layout;
    }
    if (!depth.isEmpty()) {
        if (!text.isEmpty()) {
            text += QStringLiteral("\n\n");
        }
        text += QString(depthKey) + QStringLiteral(": ") + depth;
    }
    return text;
}

QString fromDepthRepresentation(const QByteArray &payload, bool avc, int viewWidth)
{
    BitReader bits(payload);
    int views = 1;
    bool zAxisEqual = true;
    if (avc && bits.u(1) == 0) {
        views = bits.ue() + 1;
    }
    const bool zNearFlag = bits.u(1);
    const bool zFarFlag = bits.u(1);
    if (avc && (zNearFlag || zFarFlag)) {
        zAxisEqual = bits.u(1);
        if (zAxisEqual) {
            bits.ue(); // common_z_axis_reference_view
        }
    }
    const bool dMinFlag = bits.u(1);
    const bool dMaxFlag = bits.u(1);
    const quint32 type = bits.ue();
    if (!avc && (dMinFlag || dMaxFlag)) {
        bits.ue(); // disparity_ref_view_id
    }
    std::optional<double> zNear;
    std::optional<double> zFar;
    std::optional<double> dMin;
    std::optional<double> dMax;
    for (int view = 0; view < views && !bits.failed(); ++view) {
        if (avc) {
            bits.ue(); // depth_info_view_id
            if ((zNearFlag || zFarFlag) && !zAxisEqual) {
                bits.ue();
            }
            if (dMinFlag || dMaxFlag) {
                bits.ue();
            }
        }
        std::optional<double> values[4];
        const bool flags[4] = {zNearFlag, zFarFlag, dMinFlag, dMaxFlag};
        for (int i = 0; i < 4; ++i) {
            if (flags[i] && !(values[i] = element(bits))) {
                return {};
            }
        }
        if (view == 0) {
            zNear = values[0];
            zFar = values[1];
            dMin = values[2];
            dMax = values[3];
        }
    }
    QList<quint32> model;
    if (type == 3) {
        const quint32 segments = bits.ue() + 1;
        if (segments > 64) {
            return {};
        }
        model.append(0);
        for (quint32 i = 1; i <= segments; ++i) {
            model.append(bits.ue());
        }
        model.append(0);
    }
    if (bits.failed() || type > 3) {
        return {};
    }

    static const char *const kinds[] = {"inverseZ", "disparity", "z", "nonlinearDisparity"};
    QString mark = QString::fromLatin1(kinds[type]);
    const auto key = [&mark](const char *name, double value) {
        mark += QLatin1Char(' ') + QLatin1String(name) + QLatin1Char('=') + number(value);
    };
    if (type == 0 || type == 2) {
        if (zNear) {
            key("near", *zNear);
        }
        if (zFar) {
            key("far", *zFar);
        }
    } else {
        if (dMin) {
            key("min", *dMin);
        }
        if (dMax) {
            key("max", *dMax);
        }
        if (viewWidth > 0) {
            key("width", viewWidth);
        }
        if (type == 3) {
            QStringList list;
            for (const quint32 value : std::as_const(model)) {
                list.append(QString::number(value));
            }
            mark += QStringLiteral(" model=") + list.join(QLatin1Char(','));
        }
    }
    return mark;
}

QString fromAuxSubtype(const QByteArray &subtype, int viewWidth)
{
    const qsizetype total = qMin<qsizetype>(be32(subtype, 0), subtype.size() - 4);
    qsizetype pos = 4;
    while (pos + 4 < 4 + total) {
        const qsizetype length = be32(subtype, pos);
        if (length < 2 || pos + 4 + length > subtype.size()) {
            return {};
        }
        const QByteArray nal = subtype.mid(pos + 4, length);
        pos += 4 + length;
        const quint8 first = quint8(nal.at(0));
        bool avc = false;
        qsizetype header = 2;
        const int hevcType = (first >> 1) & 0x3f;
        if ((first & 0x80) || (hevcType != 39 && hevcType != 40)) {
            if ((first & 0x1f) != 6) {
                continue;
            }
            avc = true;
            header = 1;
        }
        const QByteArray rbsp = unescape(nal.mid(header));
        qsizetype at = 0;
        while (at < rbsp.size() && !(at == rbsp.size() - 1 && quint8(rbsp.at(at)) == 0x80)) {
            int payloadType = 0;
            int payloadSize = 0;
            while (at < rbsp.size() && quint8(rbsp.at(at)) == 0xff) {
                payloadType += 255;
                ++at;
            }
            if (at >= rbsp.size()) {
                break;
            }
            payloadType += quint8(rbsp.at(at++));
            while (at < rbsp.size() && quint8(rbsp.at(at)) == 0xff) {
                payloadSize += 255;
                ++at;
            }
            if (at >= rbsp.size()) {
                break;
            }
            payloadSize += quint8(rbsp.at(at++));
            if (at + payloadSize > rbsp.size()) {
                break;
            }
            if (payloadType == (avc ? 50 : 177)) {
                return fromDepthRepresentation(rbsp.mid(at, payloadSize), avc, viewWidth);
            }
            at += payloadSize;
        }
    }
    return {};
}

QByteArray auxSubtype(const QByteArray &file, quint32 itemId)
{
    std::optional<Box> meta;
    for (const Box &box : boxes(file, 0, file.size())) {
        if (box.type == "meta") {
            meta = box;
            break;
        }
    }
    if (!meta) {
        return {};
    }
    const auto iprp = child(file, *meta, "iprp", 4);
    if (!iprp) {
        return {};
    }
    const auto ipco = child(file, *iprp, "ipco");
    const auto ipma = child(file, *iprp, "ipma");
    if (!ipco || !ipma) {
        return {};
    }
    const QList<Box> properties = boxes(file, ipco->payload, ipco->end);
    const int version = quint8(file.at(ipma->payload));
    const bool wide = be32(file, ipma->payload) & 1;
    qsizetype pos = ipma->payload + 4;
    const quint32 entries = be32(file, pos);
    pos += 4;
    for (quint32 i = 0; i < entries && pos < ipma->end; ++i) {
        const quint32 id = version < 1 ? be16(file, pos) : be32(file, pos);
        pos += version < 1 ? 2 : 4;
        const int count = pos < ipma->end ? quint8(file.at(pos)) : 0;
        ++pos;
        for (int j = 0; j < count && pos < ipma->end; ++j) {
            const int index = wide ? be16(file, pos) & 0x7fff : quint8(file.at(pos)) & 0x7f;
            pos += wide ? 2 : 1;
            if (id != itemId || index < 1 || index > properties.size() || properties.at(index - 1).type != "auxC") {
                continue;
            }
            const Box &auxC = properties.at(index - 1);
            const qsizetype nameEnd = file.indexOf('\0', auxC.payload + 4);
            if (nameEnd < 0 || nameEnd >= auxC.end) {
                return {};
            }
            return file.mid(nameEnd + 1, auxC.end - nameEnd - 1);
        }
    }
    return {};
}

bool mayHaveSecondImage(const QByteArray &file)
{
    for (const Box &meta : boxes(file, 0, file.size())) {
        if (meta.type != "meta") {
            continue;
        }
        for (const Box &box : boxes(file, meta.payload + 4, meta.end)) {
            const qsizetype children = box.type == "iref" ? box.payload + 4 : box.type == "grpl" ? box.payload : -1;
            if (children < 0) {
                continue;
            }
            for (const Box &entry : boxes(file, children, box.end)) {
                if (entry.type == "auxl" || entry.type == "ster") {
                    return true;
                }
            }
        }
        return false;
    }
    return false;
}

void Xmp::add(const QByteArray &packet)
{
    enum class Context {
        None,
        Item,
        Camera,
        DepthMap,
        Image,
        Profile,
        CameraIndices,
    };
    struct Open {
        Context context;
        int depth;
    };
    QList<Open> open;
    QString profileType;
    QList<int> profileCameras;
    QString text;
    bool leaf = false;
    int depth = 0;

    const auto inside = [&open](Context context) {
        for (const Open &o : std::as_const(open)) {
            if (o.context == context) {
                return true;
            }
        }
        return false;
    };
    const auto assign = [&](const QString &ns, const QString &name, const QString &value) {
        m_values.insert(ns + name, m_values.value(ns + name, value));
        if (ns == itemNs && inside(Context::Item) && !items.isEmpty()) {
            Item &item = items.last();
            if (name == QLatin1String("Mime")) {
                item.mime = value;
            } else if (name == QLatin1String("Length")) {
                item.length = value.toLongLong();
            } else if (name == QLatin1String("Padding")) {
                item.padding = value.toLongLong();
            } else if (name == QLatin1String("DataURI")) {
                item.dataUri = value;
            }
        } else if (ns == depthMapNs && inside(Context::DepthMap) && !depthMaps.isEmpty()) {
            DepthMap &map = depthMaps.last();
            map.valid = true;
            if (name == QLatin1String("Format")) {
                map.format = value;
            } else if (name == QLatin1String("ItemSemantic")) {
                map.semantic = value;
            } else if (name == QLatin1String("Near")) {
                map.near = value.toDouble();
            } else if (name == QLatin1String("Far")) {
                map.far = value.toDouble();
            } else if (name == QLatin1String("Units")) {
                map.units = value;
            } else if (name == QLatin1String("DepthURI")) {
                map.depthUri = value;
            } else if (name == QLatin1String("MeasureType")) {
                map.measureType = value;
            }
        } else if (ns == imageNs && inside(Context::Image) && !images.isEmpty()) {
            if (name == QLatin1String("ItemSemantic")) {
                images.last().first = value;
            } else if (name == QLatin1String("ItemURI")) {
                images.last().second = value;
            }
        } else if (ns == profileNs && inside(Context::Profile) && name == QLatin1String("Type")) {
            profileType = value;
        }
    };

    QXmlStreamReader xml(packet);
    while (!xml.atEnd()) {
        switch (xml.readNext()) {
        case QXmlStreamReader::StartElement: {
            ++depth;
            const QString ns = xml.namespaceUri().toString();
            const QString name = xml.name().toString();
            if (ns == containerNs && name == QLatin1String("Item")) {
                items.append(Item());
                open.append({Context::Item, depth});
            } else if (ns == deviceNs && name == QLatin1String("Camera")) {
                depthMaps.append(DepthMap());
                images.append(QPair<QString, QString>());
                open.append({Context::Camera, depth});
            } else if (ns == cameraNs && name == QLatin1String("DepthMap")) {
                open.append({Context::DepthMap, depth});
            } else if (ns == cameraNs && name == QLatin1String("Image")) {
                open.append({Context::Image, depth});
            } else if (ns == deviceNs && name == QLatin1String("Profile")) {
                profileType.clear();
                profileCameras.clear();
                open.append({Context::Profile, depth});
            } else if (ns == profileNs && name == QLatin1String("CameraIndices")) {
                open.append({Context::CameraIndices, depth});
            }
            for (const QXmlStreamAttribute &attribute : xml.attributes()) {
                if (!attribute.namespaceUri().isEmpty()) {
                    assign(attribute.namespaceUri().toString(), attribute.name().toString(), attribute.value().toString());
                }
            }
            text.clear();
            leaf = true;
            break;
        }
        case QXmlStreamReader::Characters:
            text += xml.text();
            break;
        case QXmlStreamReader::EndElement: {
            const QString value = text.trimmed();
            if (leaf && !value.isEmpty()) {
                if (inside(Context::CameraIndices) && xml.name() == QLatin1String("li")) {
                    profileCameras.append(value.toInt());
                } else {
                    assign(xml.namespaceUri().toString(), xml.name().toString(), value);
                }
            }
            if (!open.isEmpty() && open.last().depth == depth) {
                if (open.last().context == Context::Profile && profileType == QLatin1String("DepthPhoto")) {
                    depthPhotoCameras = profileCameras;
                }
                open.removeLast();
            }
            text.clear();
            leaf = false;
            --depth;
            break;
        }
        default:
            break;
        }
    }
}

QString Xmp::value(QStringView ns, QStringView name) const
{
    return m_values.value(ns.toString() + name.toString());
}

bool Xmp::has(QStringView ns) const
{
    const QString prefix = ns.toString();
    for (auto it = m_values.cbegin(); it != m_values.cend(); ++it) {
        if (it.key().startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

QByteArray extendedXmp(const QList<QByteArray> &chunks, const QByteArray &guid)
{
    QByteArray result;
    quint32 total = 0;
    for (const QByteArray &chunk : chunks) {
        if (chunk.size() < 40 || chunk.left(32) != guid) {
            continue;
        }
        const quint32 length = be32(chunk, 32);
        const quint32 offset = be32(chunk, 36);
        if (total == 0) {
            if (length > 256 * 1024 * 1024) {
                return {};
            }
            total = length;
            result = QByteArray(int(length), '\0');
        }
        if (length != total || qint64(offset) + chunk.size() - 40 > total) {
            return {};
        }
        std::copy(chunk.cbegin() + 40, chunk.cend(), result.begin() + offset);
    }
    return result;
}

bool isAppleDepth(const Xmp &xmp)
{
    const QString type = xmp.value(appleNs, u"AuxiliaryImageType");
    const QString format = fourcc(xmp.value(appleNs, u"NativeFormat"));
    if (!type.isEmpty() && type != QLatin1String("disparity") && type != QLatin1String("depth")) {
        return false;
    }
    return format == QLatin1String("hdis") || format == QLatin1String("fdis") || format == QLatin1String("hdep") || format == QLatin1String("fdep");
}

QString fromAppleDepthData(const Xmp &xmp, int bits)
{
    if (!isAppleDepth(xmp) || bits < 1 || bits > 16) {
        return {};
    }
    const QString format = fourcc(xmp.value(appleNs, u"NativeFormat"));
    bool ok[4];
    const double intMin = xmp.value(appleNs, u"IntMinValue").toDouble(&ok[0]);
    const double intMax = xmp.value(appleNs, u"IntMaxValue").toDouble(&ok[1]);
    const double floatMin = xmp.value(appleNs, u"FloatMinValue").toDouble(&ok[2]);
    const double floatMax = xmp.value(appleNs, u"FloatMaxValue").toDouble(&ok[3]);
    if (!ok[0] || !ok[1] || !ok[2] || !ok[3] || intMax == intMin) {
        return {};
    }
    // the values at the ends of the sample range, the stored range extended as it is linear
    const double maxVal = std::ldexp(1.0, bits) - 1;
    const double scale = (floatMax - floatMin) / (intMax - intMin);
    const double first = floatMin + (0 - intMin) * scale;
    const double last = floatMin + (maxVal - intMin) * scale;
    if (format.endsWith(QLatin1String("dis"))) {
        // disparity = 1/Z: H.265 Table G.3 type 0, 0 at 1/ZFar and maxVal at 1/ZNear. A disparity of 0 is infinitely
        // far; it is kept as a very far plane.
        const double tiny = 1e-9 * qMax(std::abs(first), std::abs(last));
        if (last <= 0 && first <= 0) {
            return {};
        }
        return QStringLiteral("inverseZ near=%1 far=%2").arg(number(1.0 / qMax(last, tiny)), number(1.0 / qMax(first, tiny)));
    }
    if (first <= 0 && last <= 0) {
        return {};
    }
    return QStringLiteral("rangeLinear near=%1 far=%2").arg(number(first), number(last));
}

QString fromRange(const QString &format, double near, double far)
{
    if (format == QLatin1String("RangeInverse")) {
        return QStringLiteral("rangeInverse near=%1 far=%2").arg(number(near), number(far));
    }
    if (format == QLatin1String("RangeLinear")) {
        return QStringLiteral("rangeLinear near=%1 far=%2").arg(number(near), number(far));
    }
    return {};
}

QImage grayDepth(const QImage &image)
{
    if (image.isNull()) {
        return {};
    }
    const bool deep = image.depth() > 32 || image.format() == QImage::Format_Grayscale16;
    return image.convertToFormat(deep ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8);
}
}
