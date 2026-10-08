/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>
    SPDX-License-Identifier: LGPL-2.1-or-later
*/

#include <QBuffer>
#include <QFile>
#include <QImageReader>
#include <QImageWriter>
#include <QTest>

class StereoMetadataTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void jps_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("expected");
        const QStringList layouts = {QStringLiteral("rowInterleaved"), QStringLiteral("sideBySide"), QStringLiteral("topBottom")};
        for (int layout = 1; layout <= 3; ++layout) {
            for (int left = 0; left <= 1; ++left) {
                const QString name = QStringLiteral("test_jps_%1_%2.jpg").arg(layout).arg(left);
                QTest::addRow("%s", qPrintable(name)) << name << layouts.at(layout - 1) + (left ? QStringLiteral("LeftFirst") : QStringLiteral("RightFirst"));
            }
        }
        QTest::newRow("red-left") << QStringLiteral("test_jps_anaglyph_left.jps") << QStringLiteral("anaglyphRedLeft");
        QTest::newRow("red-right") << QStringLiteral("test_jps_anaglyph_right.jps") << QStringLiteral("anaglyphRedRight");
        QTest::newRow("mono") << QStringLiteral("test_jps_mono.jpg") << QStringLiteral("mono");
        QTest::newRow("unmarked") << QStringLiteral("test_jps_unmarked.jps") << QStringLiteral("sideBySideRightFirst");
        for (const QString &kind : {QStringLiteral("badlength"), QStringLiteral("reserved"), QStringLiteral("duplicate")}) {
            QTest::addRow("%s", qPrintable(kind)) << QStringLiteral("test_jps_%1.jps").arg(kind) << QString();
        }
    }

    void jps()
    {
        QFETCH(QString, file);
        QFETCH(QString, expected);
        QFile input(QFINDTESTDATA("stereo/" + file));
        QVERIFY(input.open(QIODevice::ReadOnly));
        QByteArray data = input.readAll();
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer, "jps");
        QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), expected);
        const QImage decoded = reader.read();
        QVERIFY(!decoded.isNull());
        QCOMPARE(decoded.text(QStringLiteral("Stereo3DLayout")), expected);
        QCOMPARE(decoded.convertToFormat(QImage::Format_RGBA8888), QImage::fromData(data, "jpeg").convertToFormat(QImage::Format_RGBA8888));
        QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), expected);
    }

    void mpo_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("expected");
        for (int little = 0; little <= 1; ++little) {
            for (int right = 0; right <= 1; ++right) {
                const QString name = QStringLiteral("test_mpo_%1_%2.mpo").arg(little).arg(right);
                QTest::addRow("%s", qPrintable(name)) << name << (right ? QStringLiteral("multiViewRightFirst") : QStringLiteral("multiViewLeftFirst"));
            }
        }
        for (const QString &kind : {QStringLiteral("unknown"), QStringLiteral("equal"), QStringLiteral("composite"), QStringLiteral("multangle")}) {
            QTest::addRow("%s", qPrintable(kind)) << QStringLiteral("test_mpo_%1.mpo").arg(kind) << QString();
        }
    }

    void mpo()
    {
        QFETCH(QString, file);
        QFETCH(QString, expected);
        QImageReader reader(QFINDTESTDATA("stereo/" + file));
        QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), expected);
        QCOMPARE(reader.imageCount(), 2);
        const QImage first = reader.read();
        QVERIFY(!first.isNull());
        QVERIFY(first.pixelColor(8, 8).red() > 240);
        QCOMPARE(first.text(QStringLiteral("Stereo3DLayout")), expected);
        QVERIFY(reader.jumpToImage(1));
        const QImage second = reader.read();
        QVERIFY(!second.isNull());
        QVERIFY(second.pixelColor(8, 8).blue() > 240);
        QCOMPARE(second.size(), first.size());
        QCOMPARE(second.text(QStringLiteral("Stereo3DLayout")), expected);
        QVERIFY(reader.jumpToImage(0));
        QCOMPARE(reader.read(), first);
        QVERIFY(!reader.jumpToImage(2));
    }

    void contentDetection_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<int>("frames");
        QTest::newRow("jps-left") << QStringLiteral("test_jps_2_1.jpg") << QStringLiteral("sideBySideLeftFirst") << 1;
        QTest::newRow("jps-right") << QStringLiteral("test_jps_2_0.jpg") << QStringLiteral("sideBySideRightFirst") << 1;
        QTest::newRow("mpo-left") << QStringLiteral("test_mpo_1_0.mpo") << QStringLiteral("multiViewLeftFirst") << 2;
        QTest::newRow("mpo-right") << QStringLiteral("test_mpo_1_1.mpo") << QStringLiteral("multiViewRightFirst") << 2;
        QTest::newRow("ordinary") << QStringLiteral("test_jps_unmarked.jpg") << QString() << 1;
    }

    void contentDetection()
    {
        QFETCH(QString, file);
        QFETCH(QString, expected);
        QFETCH(int, frames);
        QFile input(QFINDTESTDATA("stereo/" + file));
        QVERIFY(input.open(QIODevice::ReadOnly));
        QByteArray data = input.readAll();
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        QImageReader reader(&buffer);
        QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), expected);
        QCOMPARE(reader.imageCount(), frames);
        QCOMPARE(reader.read().text(QStringLiteral("Stereo3DLayout")), expected);
        QImageReader named(input.fileName());
        named.setDecideFormatFromContent(true);
        QCOMPARE(named.text(QStringLiteral("Stereo3DLayout")), expected);
        QCOMPARE(named.imageCount(), frames);
    }

    void writerViewpoints()
    {
        QImage source(32, 16, QImage::Format_RGB32);
        source.fill(Qt::red);
        for (int y = 0; y < 16; ++y) {
            for (int x = 16; x < 32; ++x) {
                source.setPixelColor(x, y, Qt::blue);
            }
        }
        QByteArray data;
        QBuffer buffer(&data);
        QVERIFY(buffer.open(QIODevice::ReadWrite));
        QImageWriter writer(&buffer, "mpo");
        QVERIFY(writer.write(source));
        QVERIFY(buffer.seek(0));
        QImageReader reader(&buffer, "mpo");
        QCOMPARE(reader.text(QStringLiteral("Stereo3DLayout")), QStringLiteral("multiViewLeftFirst"));
        QCOMPARE(reader.imageCount(), 2);
        QVERIFY(reader.read().pixelColor(8, 8).red() > 240);
        QVERIFY(reader.jumpToImage(1));
        QVERIFY(reader.read().pixelColor(8, 8).blue() > 240);
    }

    void ordinary()
    {
        QImageReader reader(QFINDTESTDATA("stereo/test_jps_unmarked.jpg"));
        QVERIFY(reader.text(QStringLiteral("Stereo3DLayout")).isEmpty());
        QVERIFY(!reader.read().isNull());
    }
};

QTEST_GUILESS_MAIN(StereoMetadataTest)

#include "stereometadatatest.moc"
