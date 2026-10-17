/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-or-later OR LGPL-3.0-or-later OR LicenseRef-KDE-Accepted-LGPL
*/

#ifndef KIMG_DEPTHMARK_P_H
#define KIMG_DEPTHMARK_P_H

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QList>
#include <QString>

/*
 * Pictures that carry a depth map as an image of their own report it the way Sparky Stereo OS's stereo depth marks
 * say (stereo-layer, docs/depth-marks.md): Stereo3DLayout "depthSeparate", the depth map as the second image of the
 * reader (QImageReader::jumpToImage(1)), the picture before the camera's effects as the third when the file holds
 * one, and Stereo3DDepth, what a depth sample means. The readers never change the picture's pixels.
 */
namespace DepthMark
{
inline constexpr QLatin1StringView layoutKey("Stereo3DLayout");
inline constexpr QLatin1StringView depthKey("Stereo3DDepth");
inline constexpr QLatin1StringView separate("depthSeparate");

// A number as the marks write it: the C notation, whatever the language, as short as it reads back exactly.
QString number(double value);

// The QImageIOHandler::Description text of these marks, which QImageReader::text() reads.
QString description(const QString &layout, const QString &depth);

// The mark of a depth representation information SEI message's payload: ITU-T H.265 G.14.2.4 (payload type 177)
// or, with avc, ITU-T H.264 depth_representation_info (payload type 50, Annex H; the first view's values). viewWidth is
// the width of the view the disparities are counted in (H.265 G.14.3.4: luma samples of the coded picture with the
// auxiliary picture's ViewId). Empty when the payload is malformed.
QString fromDepthRepresentation(const QByteArray &payload, bool avc, int viewWidth);

// The mark of the first depth representation information SEI message in NAL units (H.265 or H.264, emulation
// prevention bytes included) laid out as libheif and the HEIF writers it reads lay out an auxC box's aux_subtype: a
// 32-bit length, then each NAL unit with a 32-bit length of its own.
QString fromAuxSubtype(const QByteArray &subtype, int viewWidth);

// The aux_subtype of the auxC property associated with item itemId, from an ISO/IEC 23008-12 file's MetaBox (the
// whole file or its beginning). Empty when there is none.
QByteArray auxSubtype(const QByteArray &file, quint32 itemId);

// Whether an ISO/IEC 23008-12 file (the whole file or its beginning) can have a second image as the readers report
// it: an auxiliary image reference ('auxl') or a stereo pair entity group ('ster'). Read from the MetaBox alone, so that
// files without either cost nothing more.
bool mayHaveSecondImage(const QByteArray &file);

// XMP properties, by namespace URI and name, from the main packet and its extension (XMP Specification Part 3,
// 1.1.3.1 for JPEG): attributes and simple elements both.
class Xmp
{
public:
    void add(const QByteArray &packet);
    QString value(QStringView ns, QStringView name) const;
    bool has(QStringView ns) const;

    // Dynamic Depth 1.0's Container items and its depth camera, when the packets hold a Device element.
    struct Item {
        QString mime;
        qint64 length = 0;
        qint64 padding = 0;
        QString dataUri;
    };
    struct DepthMap {
        QString format;
        QString semantic;
        double near = 0;
        double far = 0;
        QString units;
        QString depthUri;
        QString measureType;
        bool valid = false;
    };
    QList<Item> items;
    QList<DepthMap> depthMaps; // by camera
    QList<QPair<QString, QString>> images; // by camera: ItemSemantic, ItemURI
    QList<int> depthPhotoCameras; // Profile "DepthPhoto"'s CameraIndices

private:
    QHash<QString, QString> m_values;
};

// Concatenates the extended XMP chunks of a JPEG file (APP1 "http://ns.adobe.com/xmp/extension/") whose GUID is
// guid, in order of their offsets.
QByteArray extendedXmp(const QList<QByteArray> &chunks, const QByteArray &guid);

// The mark of Apple's depth data description (XMP namespace http://ns.apple.com/pixeldatainfo/1.0/ on the depth
// image: NativeFormat, IntMinValue to IntMaxValue stored for FloatMinValue to FloatMaxValue): a disparity in 1/m
// (kCVPixelFormatType_DisparityFloat16 "hdis", DisparityFloat32 "fdis") is uniform in 1/Z; a depth in metres
// ("hdep", "fdep") is uniform in distance. bits is the depth image's sample depth. Empty when the XMP is not Apple's
// depth description.
QString fromAppleDepthData(const Xmp &xmp, int bits);

// Whether Apple's description is of a depth or disparity map, as opposed to the portrait matte, a segmentation
// matte or an HDR gain map, which carry no depth.
bool isAppleDepth(const Xmp &xmp);

// The mark of Google's depth map formats (Dynamic Depth 1.0 Depth Map Format; GDepth Format): RangeLinear and
// RangeInverse, both 0 at Near and the largest sample at Far.
QString fromRange(const QString &format, double near, double far);

// The depth map as it is reported: one gray channel, 16 bits when the image has more than 8.
QImage grayDepth(const QImage &image);
}

#endif // KIMG_DEPTHMARK_P_H
