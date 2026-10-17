/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-or-later OR LGPL-3.0-or-later OR LicenseRef-KDE-Accepted-LGPL
*/

#include "depthmark_p.h"
#include "heifdepth_p.h"
#include "microexif_p.h"

#include <QIODevice>
#include <QTransform>
#include <QtEndian>

#include <libheif/heif.h>
#include <libheif/heif_properties.h>

#include <cstring>

namespace
{
// The EXIF orientation of an item's transformative properties (irot, imir), 0 when it has none: the readers report
// it as QImageIOHandler::ImageTransformation and leave the pixels as coded, as the HEIF plugin does for the primary
// image.
quint16 orientation(const heif_context *ctx, heif_item_id id)
{
    heif_transform_mirror_direction mirror = heif_transform_mirror_direction_invalid;
    heif_property_id property;
    if (heif_item_get_properties_of_type(ctx, id, heif_item_property_type_transform_mirror, &property, 1) > 0) {
        mirror = heif_item_get_property_transform_mirror(ctx, id, property);
    }
    int rotation = -1;
    if (heif_item_get_properties_of_type(ctx, id, heif_item_property_type_transform_rotation, &property, 1) > 0) {
        rotation = heif_item_get_property_transform_rotation_ccw(ctx, id, property);
    }
    const bool none = mirror == heif_transform_mirror_direction_invalid;
    if (rotation <= 0 && none) {
        return rotation == 0 ? 1 : 0;
    }
    if (rotation <= 0) {
        return mirror == heif_transform_mirror_direction_horizontal ? 2 : 4;
    }
    if (rotation == 180 && none) {
        return 3;
    }
    if (rotation == 270) {
        return none ? 6 : (mirror == heif_transform_mirror_direction_horizontal ? 5 : 7);
    }
    if (rotation == 90 && none) {
        return 8;
    }
    return 0;
}

// The image as its clean aperture ('clap') keeps it: the readers decode with the transformations ignored, so that the
// rotation and the mirror are reported, as the HEIF plugin does for the primary image.
QImage cropped(const heif_context *ctx, heif_item_id id, const QImage &image)
{
    heif_property_id property;
    if (image.isNull() || heif_item_get_properties_of_type(ctx, id, heif_item_property_type_transform_crop, &property, 1) <= 0) {
        return image;
    }
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    heif_item_get_property_transform_crop_borders(ctx, id, property, image.width(), image.height(), &left, &top, &right, &bottom);
    const QRect rect(left, top, image.width() - left - right, image.height() - top - bottom);
    return rect.isValid() && image.rect().contains(rect) ? image.copy(rect) : image;
}

QImage decodeGray(heif_image_handle *handle)
{
    heif_decoding_options *options = heif_decoding_options_alloc();
    options->ignore_transformations = 1;
    heif_image *img = nullptr;
    const heif_error error = heif_decode_image(handle, &img, heif_colorspace_monochrome, heif_chroma_monochrome, options);
    heif_decoding_options_free(options);
    if (error.code || !img) {
        return {};
    }
    const int width = heif_image_get_width(img, heif_channel_Y);
    const int height = heif_image_get_height(img, heif_channel_Y);
    const int bits = heif_image_get_bits_per_pixel_range(img, heif_channel_Y);
    int stride = 0;
    const uint8_t *plane = heif_image_get_plane_readonly(img, heif_channel_Y, &stride);
    QImage result;
    if (plane && width > 0 && height > 0 && bits >= 1 && bits <= 16) {
        result = QImage(width, height, bits > 8 ? QImage::Format_Grayscale16 : QImage::Format_Grayscale8);
        const quint32 maxVal = (1u << bits) - 1;
        for (int y = 0; y < height && !result.isNull(); ++y) {
            const uint8_t *row = plane + qsizetype(y) * stride;
            if (bits > 8) {
                auto *out = reinterpret_cast<quint16 *>(result.scanLine(y));
                const auto *in = reinterpret_cast<const quint16 *>(row);
                // the samples spread over 16 bits, as a reader of a 16-bit gray image takes them (maxVal = 65535)
                for (int x = 0; x < width; ++x) {
                    out[x] = quint16((quint32(qMin<quint32>(in[x], maxVal)) * 65535u + maxVal / 2) / maxVal);
                }
            } else if (bits == 8) {
                std::memcpy(result.scanLine(y), row, width);
            } else {
                uchar *out = result.scanLine(y);
                for (int x = 0; x < width; ++x) {
                    out[x] = uchar((quint32(qMin<quint32>(row[x], maxVal)) * 255u + maxVal / 2) / maxVal);
                }
            }
        }
    }
    heif_image_release(img);
    return result;
}

QImage decodeColor(heif_image_handle *handle, const QColorSpace &colorSpace)
{
    const bool deep = heif_image_handle_get_luma_bits_per_pixel(handle) > 8;
    const heif_chroma chroma = deep
        ? (QSysInfo::ByteOrder == QSysInfo::LittleEndian ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE)
        : heif_chroma_interleaved_RGBA;
    heif_decoding_options *options = heif_decoding_options_alloc();
    options->ignore_transformations = 1;
    heif_image *img = nullptr;
    const heif_error error = heif_decode_image(handle, &img, heif_colorspace_RGB, chroma, options);
    heif_decoding_options_free(options);
    if (error.code || !img) {
        return {};
    }
    const int width = heif_image_get_width(img, heif_channel_interleaved);
    const int height = heif_image_get_height(img, heif_channel_interleaved);
    const int bits = heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved);
    int stride = 0;
    const uint8_t *plane = heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);
    QImage result;
    if (plane && width > 0 && height > 0) {
        result = QImage(width, height, deep ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
        const int shift = deep ? 16 - bits : 0;
        for (int y = 0; y < height && !result.isNull(); ++y) {
            const uint8_t *row = plane + qsizetype(y) * stride;
            if (deep) {
                const auto *in = reinterpret_cast<const quint16 *>(row);
                auto *out = reinterpret_cast<quint16 *>(result.scanLine(y));
                for (int x = 0; x < width * 4; ++x) {
                    out[x] = quint16((in[x] << shift) | (shift > 0 ? in[x] >> (bits - shift) : 0));
                }
            } else {
                std::memcpy(result.scanLine(y), row, qsizetype(width) * 4);
            }
        }
        result.setColorSpace(colorSpace);
    }
    heif_image_release(img);
    return result;
}

bool isDepthType(const char *type)
{
    return type
        && (!std::strcmp(type, "urn:mpeg:hevc:2015:auxid:2") || !std::strcmp(type, "urn:mpeg:avc:2015:auxid:2")
            || !std::strcmp(type, "urn:mpeg:mpegB:cicp:systems:auxiliary:depth"));
}

QByteArray xmpOf(heif_image_handle *handle)
{
    const int count = heif_image_handle_get_number_of_metadata_blocks(handle, nullptr);
    if (count <= 0 || count > 64) {
        return {};
    }
    QList<heif_item_id> ids(count);
    heif_image_handle_get_list_of_metadata_block_IDs(handle, nullptr, ids.data(), count);
    for (const heif_item_id id : std::as_const(ids)) {
        const char *contentType = heif_image_handle_get_metadata_content_type(handle, id);
        const size_t size = heif_image_handle_get_metadata_size(handle, id);
        if (contentType && !std::strcmp(contentType, "application/rdf+xml") && size > 0 && size < 16 * 1024 * 1024) {
            QByteArray xmp(qsizetype(size), '\0');
            if (!heif_image_handle_get_metadata(handle, id, xmp.data()).code) {
                return xmp;
            }
        }
    }
    return {};
}

// libheif's reading of the depth representation information, for files whose aux_subtype the reader could not
// parse itself; libheif keeps no nonlinear model.
QString libheifMark(heif_image_handle *primary, heif_item_id depthId, int viewWidth)
{
    const heif_depth_representation_info *info = nullptr;
    if (!heif_image_handle_get_depth_image_representation_info(primary, depthId, &info) || !info) {
        return {};
    }
    static const char *const kinds[] = {"inverseZ", "disparity", "z", "nonlinearDisparity"};
    QString mark;
    const int type = int(info->depth_representation_type);
    if (type >= 0 && type <= 2) {
        mark = QString::fromLatin1(kinds[type]);
        const auto key = [&mark](const char *name, double value) {
            mark += QLatin1Char(' ') + QLatin1String(name) + QLatin1Char('=') + DepthMark::number(value);
        };
        if (type == 1) {
            if (info->has_d_min) {
                key("min", info->d_min);
            }
            if (info->has_d_max) {
                key("max", info->d_max);
            }
            key("width", viewWidth);
        } else {
            if (info->has_z_near) {
                key("near", info->z_near);
            }
            if (info->has_z_far) {
                key("far", info->z_far);
            }
        }
    }
    heif_depth_representation_info_free(info);
    return mark;
}

HeifSecondImage stereoPair(heif_context *ctx, heif_image_handle *primary, quint16 primaryOrientation, const QColorSpace &colorSpace)
{
    HeifSecondImage result;
#if LIBHEIF_HAVE_VERSION(1, 19, 0)
    const heif_item_id primaryId = heif_image_handle_get_item_id(primary);
    int groups = 0;
    heif_entity_group *group = heif_context_get_entity_groups(ctx, heif_fourcc('s', 't', 'e', 'r'), primaryId, &groups);
    for (int i = 0; i < groups && result.image.isNull(); ++i) {
        if (group[i].num_entities != 2 || (group[i].entities[0] != primaryId && group[i].entities[1] != primaryId)) {
            continue;
        }
        const bool primaryIsLeft = group[i].entities[0] == primaryId;
        const heif_item_id other = primaryIsLeft ? group[i].entities[1] : group[i].entities[0];
        heif_image_handle *handle = nullptr;
        if (heif_context_get_image_handle(ctx, other, &handle).code) {
            continue;
        }
        result.image = cropped(ctx, other, decodeColor(handle, colorSpace));
        heif_image_handle_release(handle);
        if (!result.image.isNull()) {
            result.layout = primaryIsLeft ? QStringLiteral("multiViewLeftFirst") : QStringLiteral("multiViewRightFirst");
            result.orientation = orientation(ctx, other);
            if (result.orientation == 0) {
                result.orientation = primaryOrientation;
            }
        }
    }
    heif_entity_groups_release(group, groups);
#else
    Q_UNUSED(ctx)
    Q_UNUSED(primary)
    Q_UNUSED(primaryOrientation)
    Q_UNUSED(colorSpace)
#endif
    return result;
}
}

HeifSecondImage
readHeifSecondImage(heif_context *ctx, heif_image_handle *primary, const QByteArray &metaBox, quint16 primaryOrientation, const QColorSpace &colorSpace)
{
    HeifSecondImage result = stereoPair(ctx, primary, primaryOrientation, colorSpace);
    if (!result.image.isNull()) {
        return result;
    }

    const int count = heif_image_handle_get_number_of_auxiliary_images(primary, 0);
    if (count <= 0 || count > 64) {
        return result;
    }
    QList<heif_item_id> ids(count);
    heif_image_handle_get_list_of_auxiliary_image_IDs(primary, 0, ids.data(), count);
    for (const heif_item_id id : std::as_const(ids)) {
        heif_image_handle *handle = nullptr;
        if (heif_image_handle_get_auxiliary_image_handle(primary, id, &handle).code) {
            continue;
        }
        const char *type = nullptr;
        const bool depth = !heif_image_handle_get_auxiliary_type(handle, &type).code && isDepthType(type);
        heif_image_handle_release_auxiliary_type(handle, &type);
        if (!depth) {
            heif_image_handle_release(handle);
            continue;
        }
        const QImage image = cropped(ctx, id, decodeGray(handle));
        const int bits = heif_image_handle_get_luma_bits_per_pixel(handle);
        DepthMark::Xmp xmp;
        xmp.add(xmpOf(handle));
        heif_image_handle_release(handle);
        if (image.isNull()) {
            continue;
        }

        const int viewWidth = heif_image_handle_get_ispe_width(primary);
        // Apple's stored range is of the coded samples, which the reported depth map spreads over 8 or 16 bits
        QString mark = DepthMark::fromAppleDepthData(xmp, bits);
        if (mark.isEmpty()) {
            mark = DepthMark::fromAuxSubtype(DepthMark::auxSubtype(metaBox, id), viewWidth);
        }
        if (mark.isEmpty()) {
            mark = libheifMark(primary, id, viewWidth);
        }
        if (mark.isEmpty()) {
            mark = QStringLiteral("nearness");
        }
        result.layout = DepthMark::separate;
        result.depthMark = mark;
        result.image = image;
        result.orientation = orientation(ctx, id);
        if (result.orientation == 0) {
            result.orientation = primaryOrientation;
        }
        break;
    }
    return result;
}

quint16 heifOrientation(const heif_context *ctx, quint32 itemId)
{
    return orientation(ctx, itemId);
}

QImage applyOrientation(const QImage &image, quint16 orientation)
{
    // as qt_imageTransform(): the mirror and the flip, then the rotation by 90 degrees clockwise
    const auto transformation = MicroExif::orientationToTransformation(orientation);
    if (transformation == QImageIOHandler::TransformationNone || image.isNull()) {
        return image;
    }
    if (transformation == QImageIOHandler::TransformationRotate270) {
        return image.transformed(QTransform().rotate(270));
    }
    Qt::Orientations flip;
    if (transformation & QImageIOHandler::TransformationMirror) {
        flip |= Qt::Horizontal;
    }
    if (transformation & QImageIOHandler::TransformationFlip) {
        flip |= Qt::Vertical;
    }
    QImage result = flip ? image.flipped(flip) : image;
    if (transformation & QImageIOHandler::TransformationRotate90) {
        result = result.transformed(QTransform().rotate(90));
    }
    return result;
}

QByteArray heifMetaBox(QIODevice *device, const QByteArray &data)
{
    if (!data.isEmpty() || !device || device->isSequential()) {
        return data;
    }
    const qint64 start = device->pos();
    QByteArray result;
    qint64 pos = 0;
    while (device->seek(pos)) {
        const QByteArray header = device->read(16);
        if (header.size() < 8) {
            break;
        }
        quint64 size = qFromBigEndian<quint32>(header.constData());
        if (size == 1 && header.size() == 16) {
            size = qFromBigEndian<quint64>(header.constData() + 8);
        } else if (size == 0) {
            size = quint64(device->size() - pos);
        }
        if (size < 8) {
            break;
        }
        if (header.mid(4, 4) == "meta") {
            device->seek(pos);
            result = device->read(qMin<qint64>(qint64(size), 64 * 1024 * 1024));
            break;
        }
        pos += qint64(size);
    }
    device->seek(start);
    return result;
}
