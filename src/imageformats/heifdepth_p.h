/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-or-later OR LGPL-3.0-or-later OR LicenseRef-KDE-Accepted-LGPL
*/

#ifndef KIMG_HEIFDEPTH_P_H
#define KIMG_HEIFDEPTH_P_H

#include <QByteArray>
#include <QColorSpace>
#include <QImage>
#include <QString>

struct heif_context;
struct heif_image_handle;
class QIODevice;

/*
 * The second image of an ISO/IEC 23008-12 file (HEIF, AVIF) whose primary image has one, read with libheif:
 *  - the other view of a stereo pair entity group, 'ster' (ISO/IEC 23008-12:2025 6.8.5: the left view, then the right
 *    view), reported as multiViewLeftFirst or multiViewRightFirst after the primary image;
 *  - else the primary image's depth map, an auxiliary image of type urn:mpeg:hevc:2015:auxid:2,
 *    urn:mpeg:avc:2015:auxid:2 or urn:mpeg:mpegB:cicp:systems:auxiliary:depth, reported as depthSeparate with its
 *    Stereo3DDepth mark: Apple's depth description in the depth image's XMP, else the depth representation information
 *    SEI message of its auxC aux_subtype (ITU-T H.265 G.14.2.4, H.264 Annex H), else nearness.
 * Mattes and gain maps are auxiliary images too, and carry no depth: they are not read.
 */
struct HeifSecondImage {
    QString layout;
    QString depthMark;
    QImage image;
    quint16 orientation = 0; // EXIF orientation; 0 for none
};

HeifSecondImage
readHeifSecondImage(heif_context *ctx, heif_image_handle *primary, const QByteArray &metaBox, quint16 primaryOrientation, const QColorSpace &colorSpace);

// The EXIF orientation of an item's transformative properties (irot, imir), 0 when it has none.
quint16 heifOrientation(const heif_context *ctx, quint32 itemId);

// image with an EXIF orientation applied, as QImageReader applies it.
QImage applyOrientation(const QImage &image, quint16 orientation);

// The file's MetaBox, read from device without moving it (or taken from data, the whole file, when it is there).
QByteArray heifMetaBox(QIODevice *device, const QByteArray &data);

#endif // KIMG_HEIFDEPTH_P_H
