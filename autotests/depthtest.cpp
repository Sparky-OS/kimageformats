/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QTest>

#include <cstring>

#ifdef KIMG_DEPTHTEST_LIBHEIF
#include <libheif/heif.h>
#endif

// Pictures with their depth map (depth/generate.py): each reader reports Stereo3DLayout "depthSeparate", the depth map
// as its second image and Stereo3DDepth, the meaning of a sample, from the file's own signalling; the picture's pixels
// are those of the same coded picture without a depth map; auxiliary images that are no depth map change nothing.
class DepthTest : public QObject
{
    Q_OBJECT

    // a device that cannot seek, as a pipe or a network reply
    class Sequential : public QIODevice
    {
    public:
        explicit Sequential(const QByteArray *data)
            : m_data(*data)
        {
        }
        bool isSequential() const override
        {
            return true;
        }
        qint64 bytesAvailable() const override
        {
            return m_data.size() - m_at + QIODevice::bytesAvailable();
        }

    protected:
        qint64 readData(char *data, qint64 maxSize) override
        {
            const qint64 count = qMin(maxSize, m_data.size() - m_at);
            std::memcpy(data, m_data.constData() + m_at, count);
            m_at += count;
            return count;
        }
        qint64 writeData(const char *, qint64) override
        {
            return -1;
        }

    private:
        const QByteArray m_data;
        qint64 m_at = 0;
    };

    static QByteArray fixture(const QString &name)
    {
        QFile file(QFINDTESTDATA(QStringLiteral("depth/") + name));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    // Whether libheif, which reads the depth maps and stereo pairs of HEIF and AVIF files, decodes their coding here.
    static bool decodes(const QString &file)
    {
        if (file.endsWith(QLatin1String(".jpg"))) {
            return true;
        }
#ifdef KIMG_DEPTHTEST_LIBHEIF
        return heif_have_decoder_for_format(file.endsWith(QLatin1String(".avif")) ? heif_compression_AV1 : heif_compression_HEVC);
#else
        return false;
#endif
    }

    // the samples of the regions of generate.py: the background, the box and the disc
    static QList<int> samples(const QImage &depth)
    {
        const int w = depth.width();
        const int h = depth.height();
        QList<int> result;
        for (const QPoint &p : {QPoint(int(0.03 * w), int(0.04 * h)), QPoint(int(0.25 * w), int(0.5 * h)), QPoint(int(0.7 * w), int(0.5 * h))}) {
            result.append(depth.format() == QImage::Format_Grayscale16 ? reinterpret_cast<const quint16 *>(depth.constScanLine(p.y()))[p.x()]
                                                                       : depth.constScanLine(p.y())[p.x()]);
        }
        return result;
    }

private Q_SLOTS:
    void initTestCase()
    {
#ifdef KIMG_DEPTHTEST_LIBHEIF
        heif_init(nullptr);
#endif
    }

    void cleanupTestCase()
    {
#ifdef KIMG_DEPTHTEST_LIBHEIF
        heif_deinit();
#endif
    }

    void depth_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("mono");
        QTest::addColumn<QString>("mark");
        QTest::addColumn<QSize>("size");
        QTest::addColumn<QList<int>>("expected");
        QTest::addColumn<int>("images");

        const QList<int> nearness{64, 161, 230};
        const QList<int> range{191, 94, 25};
        const QSize full(64, 48);
        const QSize half(32, 24);
        // H.265 G.14.2.4 depth representation information in auxC, types 0 to 3 (Table G.3)
        QTest::newRow("heif inverse Z") << QStringLiteral("heif_depth_inverse_z.heic") << QStringLiteral("heif_mono.heic")
                                        << QStringLiteral("inverseZ near=0.5 far=8") << full << nearness << 2;
        QTest::newRow("heif disparity") << QStringLiteral("heif_depth_disparity.heic") << QStringLiteral("heif_mono.heic")
                                        << QStringLiteral("disparity min=-6.5 max=19.25 width=64") << full << nearness << 2;
        QTest::newRow("heif Z, half size") << QStringLiteral("heif_depth_z.heic") << QStringLiteral("heif_mono.heic") << QStringLiteral("z near=1.25 far=40")
                                           << half << nearness << 2;
        QTest::newRow("heif nonlinear") << QStringLiteral("heif_depth_nonlinear.heic") << QStringLiteral("heif_mono.heic")
                                        << QStringLiteral("nonlinearDisparity min=-4 max=12 width=64 model=0,20,7,0") << full << nearness << 2;
        // H.264 Annex H depth_representation_info, urn:mpeg:avc:2015:auxid:2
        QTest::newRow("heif H.264 syntax") << QStringLiteral("heif_depth_avc_sei.heic") << QStringLiteral("heif_mono.heic")
                                           << QStringLiteral("inverseZ near=2 far=30") << full << nearness << 2;
        QTest::newRow("heif no signalling") << QStringLiteral("heif_depth_plain.heic") << QStringLiteral("heif_mono.heic") << QStringLiteral("nearness") << full
                                            << nearness << 2;
        QTest::newRow("heif 10 bits") << QStringLiteral("heif_depth_10bit.heic") << QStringLiteral("heif_mono.heic")
                                      << QStringLiteral("inverseZ near=0.75 far=12") << full << QList<int>{16400, 41320, 59001} << 2;
        // Apple's depth description wins over its disparity SEI, whose DMin and DMax are in 1/m, not in luma samples
        QTest::newRow("heif Apple disparity") << QStringLiteral("heif_depth_apple.heic") << QStringLiteral("heif_mono.heic")
                                              << QStringLiteral("inverseZ near=0.2222222222222222 far=0.6666666666666666") << full << nearness << 2;
        QTest::newRow("heif Apple depth") << QStringLiteral("heif_depth_apple_metres.heic") << QStringLiteral("heif_mono.heic")
                                          << QStringLiteral("rangeLinear near=0.4 far=3.5") << full << nearness << 2;
        QTest::newRow("avif") << QStringLiteral("avif_depth.avif") << QStringLiteral("avif_mono.avif") << QStringLiteral("inverseZ near=1 far=20") << full
                              << nearness << 2;
        QTest::newRow("avif no signalling") << QStringLiteral("avif_depth_plain.avif") << QStringLiteral("avif_mono.avif") << QStringLiteral("nearness") << full
                                            << nearness << 2;
        // Dynamic Depth 1.0, attributes and elements, a depth JPEG or a 16-bit PNG, Padding, the Original image
        QTest::newRow("dynamic depth") << QStringLiteral("jpeg_dynamic_depth.jpg") << QStringLiteral("jpeg_mono.jpg")
                                       << QStringLiteral("rangeInverse near=0.3 far=6") << half << range << 3;
        QTest::newRow("dynamic depth png16") << QStringLiteral("jpeg_dynamic_depth_png16.jpg") << QStringLiteral("jpeg_mono.jpg")
                                             << QStringLiteral("rangeLinear near=0.5 far=9") << half << QList<int>{49151, 24248, 6553} << 2;
        QTest::newRow("dynamic depth padding") << QStringLiteral("jpeg_dynamic_depth_padding.jpg") << QStringLiteral("jpeg_mono.jpg")
                                               << QStringLiteral("rangeInverse near=0.3 far=6") << half << range << 2;
        QTest::newRow("dynamic depth rotated") << QStringLiteral("jpeg_dynamic_depth_rotated.jpg") << QStringLiteral("")
                                               << QStringLiteral("rangeInverse near=0.3 far=6") << half << range << 2;
        // GDepth: the depth map in the extended XMP, GImage the original
        QTest::newRow("gdepth inverse") << QStringLiteral("jpeg_gdepth_inverse.jpg") << QStringLiteral("jpeg_mono.jpg")
                                        << QStringLiteral("rangeInverse near=0.4 far=5") << half << range << 3;
        QTest::newRow("gdepth linear") << QStringLiteral("jpeg_gdepth_linear.jpg") << QStringLiteral("jpeg_mono.jpg")
                                       << QStringLiteral("rangeLinear near=1 far=3") << half << range << 2;
        // Apple's Multi-Picture Format JPEG: a gain map, then the disparity map
        QTest::newRow("apple jpeg") << QStringLiteral("jpeg_apple_depth.jpg") << QStringLiteral("jpeg_mono.jpg")
                                    << QStringLiteral("inverseZ near=0.20833333333333334 far=4") << half << nearness << 2;
    }

    void depth()
    {
        QFETCH(QString, file);
        QFETCH(QString, mono);
        QFETCH(QString, mark);
        QFETCH(QSize, size);
        QFETCH(QList<int>, expected);
        QFETCH(int, images);

        if (!decodes(file)) {
            QSKIP("libheif has no decoder for this coding here");
        }
        QByteArray data = fixture(file);
        QVERIFY(!data.isEmpty());
        for (const bool sequential : {false, true}) {
            QBuffer random(&data);
            Sequential pipe(&data);
            QIODevice *device = sequential ? static_cast<QIODevice *>(&pipe) : &random;
            QVERIFY(device->open(QIODevice::ReadOnly));
            QImageReader reader(device);
            QVERIFY(reader.canRead());
            QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), QStringLiteral("depthSeparate"));
            QCOMPARE(reader.text(QStringLiteral("Stereo3DDepth")), mark);
            QCOMPARE(reader.imageCount(), images);
            const QImage picture = reader.read();
            QVERIFY(!picture.isNull());
            QCOMPARE(picture.text(QStringLiteral("Stereo3DLayout")), QStringLiteral("depthSeparate"));
            QCOMPARE(picture.text(QStringLiteral("Stereo3DDepth")), mark);
            if (!mono.isEmpty()) {
                const QImage plain = QImage::fromData(fixture(mono));
                QCOMPARE(picture.convertToFormat(QImage::Format_RGBA8888), plain.convertToFormat(QImage::Format_RGBA8888));
            }
            QVERIFY(reader.jumpToImage(1));
            QCOMPARE(reader.size(), size);
            const QImage depth = reader.read();
            QCOMPARE(depth.size(), size);
            QVERIFY(depth.format() == QImage::Format_Grayscale8 || depth.format() == QImage::Format_Grayscale16);
            QCOMPARE(samples(depth), expected);
            if (images > 2) {
                QVERIFY(reader.jumpToImage(2));
                QCOMPARE(reader.read().size(), picture.size());
            }
            QVERIFY(!reader.jumpToImage(images));
        }
    }

    // The EXIF orientation of a Dynamic Depth JPEG applies to its depth map too.
    void orientation()
    {
        QByteArray data = fixture(QStringLiteral("jpeg_dynamic_depth_rotated.jpg"));
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        QCOMPARE(reader.transformation(), QImageIOHandler::TransformationRotate90);
        QCOMPARE(reader.read().size(), QSize(48, 64));
        QVERIFY(reader.jumpToImage(1));
        QCOMPARE(reader.transformation(), QImageIOHandler::TransformationRotate90);
        QCOMPARE(reader.read().size(), QSize(24, 32));
    }

    // Stereo pairs of ISO/IEC 23008-12:2025 6.8.5: the left view, then the right view.
    void stereoPair_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("layout");
        QTest::newRow("left primary") << QStringLiteral("heif_stereo_left_primary.heic") << QStringLiteral("multiViewLeftFirst");
        QTest::newRow("right primary") << QStringLiteral("heif_stereo_right_primary.heic") << QStringLiteral("multiViewRightFirst");
    }

    void stereoPair()
    {
        QFETCH(QString, file);
        QFETCH(QString, layout);
        if (!decodes(file)) {
            QSKIP("libheif has no decoder for this coding here");
        }
        QByteArray data = fixture(file);
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer);
        QVERIFY(reader.canRead());
        QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), layout);
        QCOMPARE(reader.imageCount(), 2);
        const QImage first = reader.read();
        QVERIFY(reader.jumpToImage(1));
        const QImage second = reader.read();
        QCOMPARE(first.convertToFormat(QImage::Format_RGBA8888),
                 QImage::fromData(fixture(QStringLiteral("heif_mono.heic"))).convertToFormat(QImage::Format_RGBA8888));
        QCOMPARE(second.size(), first.size());
        // the other view is the picture's negative (generate.py)
        const QColor a = first.pixelColor(10, 10);
        const QColor b = second.pixelColor(10, 10);
        QVERIFY(qAbs(a.red() + b.red() - 255) < 24);
    }

    // The controls: no depth map, so an ordinary picture: a portrait matte, a gain map, a segmentation mask and plain
    // pictures are read as before and never as a picture with its depth.
    void noDepth_data()
    {
        QTest::addColumn<QString>("file");
        for (const char *name : {"heif_mono.heic",
                                 "avif_mono.avif",
                                 "heif_matte_apple.heic",
                                 "heif_gainmap_apple.heic",
                                 "jpeg_mono.jpg",
                                 "jpeg_apple_matte.jpg",
                                 "jpeg_gainmap.jpg",
                                 "jpeg_dynamic_depth_segmentation.jpg"}) {
            QTest::newRow(name) << QString::fromLatin1(name);
        }
    }

    void noDepth()
    {
        QFETCH(QString, file);
        QByteArray data = fixture(file);
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer);
        if (!reader.canRead()) {
            QSKIP("no decoder for this format here");
        }
        QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), QString());
        QCOMPARE(reader.text(QStringLiteral("Stereo3DDepth")), QString());
        QCOMPARE(reader.imageCount(), 1);
        QCOMPARE(reader.read().size(), QSize(64, 48));
    }
};

QTEST_GUILESS_MAIN(DepthTest)

#include "depthtest.moc"
