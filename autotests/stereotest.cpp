/*
    SPDX-FileCopyrightText: 2026 The KDE Contributors

    SPDX-License-Identifier: LGPL-2.1-or-later OR LGPL-3.0-or-later OR LicenseRef-KDE-Accepted-LGPL
*/

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QDebug>
#include <QFile>
#include <QFileInfo>

namespace
{
bool check(bool condition, const char *message)
{
    if (!condition) {
        qCritical() << message;
    }
    return condition;
}

bool isMostly(const QColor &actual, const QColor &expected)
{
    return qAbs(actual.red() - expected.red()) < 12
        && qAbs(actual.green() - expected.green()) < 12
        && qAbs(actual.blue() - expected.blue()) < 12;
}

QImage stereoSource()
{
    QImage image(8, 4, QImage::Format_RGB888);
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            image.setPixelColor(x, y, x < image.width() / 2 ? Qt::red : Qt::blue);
        }
    }
    return image;
}

bool testJps()
{
    const QImage source = stereoSource();
    QByteArray data;
    QBuffer output(&data);
    output.open(QIODevice::WriteOnly);
    QImageWriter writer(&output, "jps");
    writer.setQuality(100);
    if (!check(writer.write(source), "JPS write failed")) {
        return false;
    }
    QFile jpsEvidence(QStringLiteral("evidence/kimageformats-test.jps"));
    if (jpsEvidence.open(QIODevice::WriteOnly)) {
        jpsEvidence.write(data);
    }

    QBuffer input(&data);
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input, "jps");
    const QImage image = reader.read();
    return check(!image.isNull(), "JPS read failed")
        && check(image.size() == source.size(), "JPS dimensions changed")
        && check(isMostly(image.pixelColor(1, 1), Qt::blue), "JPS did not put right eye first")
        && check(isMostly(image.pixelColor(5, 1), Qt::red), "JPS did not put left eye second")
        && check(image.text(QStringLiteral("StereoLayout")).contains(QStringLiteral("right eye first")), "JPS layout metadata missing");
}

bool testMpo()
{
    const QImage source = stereoSource();
    source.copy(0, 0, source.width() / 2, source.height()).save(QStringLiteral("evidence/kimageformats-left.jpg"), "jpeg", 100);
    source.copy(source.width() / 2, 0, source.width() / 2, source.height()).save(QStringLiteral("evidence/kimageformats-right.jpg"), "jpeg", 100);
    QByteArray data;
    QBuffer output(&data);
    output.open(QIODevice::WriteOnly);
    QImageWriter writer(&output, "mpo");
    writer.setQuality(100);
    if (!check(writer.write(source), "MPO write failed")) {
        return false;
    }
    QFile evidence(QStringLiteral("evidence/kimageformats-test.mpo"));
    if (evidence.open(QIODevice::WriteOnly)) {
        evidence.write(data);
    }

    QBuffer input(&data);
    input.open(QIODevice::ReadOnly);
    QImageReader reader(&input, "mpo");
    if (!check(reader.imageCount() == 2, "MPO image count is not two")) {
        return false;
    }
    const QImage left = reader.read();
    const bool leftOk = check(isMostly(left.pixelColor(1, 1), Qt::red), "MPO first image is not the left eye");
    if (!check(reader.jumpToImage(1), "MPO second image is not seekable")) {
        return false;
    }
    const QImage right = reader.read();
    return leftOk
        && check(isMostly(right.pixelColor(1, 1), Qt::blue), "MPO second image is not the right eye")
        && check(right.text(QStringLiteral("StereoLayout")).contains(QStringLiteral("left eye first")), "MPO layout metadata missing");
}

bool testExternalMpo(const QString &path)
{
    QImageReader reader(path, "mpo");
    if (!check(reader.imageCount() == 2, "external MPO image count is not two")) {
        return false;
    }
    const QImage first = reader.read();
    if (!check(reader.jumpToImage(1), "external MPO second image is not seekable")) {
        return false;
    }
    const QImage second = reader.read();
    return check(isMostly(first.pixelColor(1, 1), Qt::red), "external MPO first image is not the left eye")
        && check(isMostly(second.pixelColor(1, 1), Qt::blue), "external MPO second image is not the right eye");
}

bool testExternalJps(const QString &path)
{
    QImageReader reader(path, "jps");
    const QImage image = reader.read();
    if (!check(!image.isNull(), "external JPS read failed") || !check(image.width() % 2 == 0, "external JPS width is not doubled")) {
        return false;
    }
    const int eyeWidth = image.width() / 2;
    const QString stem = path.contains(QStringLiteral("wiz3D"), Qt::CaseInsensitive) ? QStringLiteral("wiz3d-test") : QStringLiteral("iz3d-test");
    const QSize evidenceSize(320, qMax(1, image.height() * 320 / eyeWidth));
    const bool saved = image.copy(0, 0, eyeWidth, image.height()).scaled(evidenceSize).save(QStringLiteral("evidence/%1-right-first.png").arg(stem))
        && image.copy(eyeWidth, 0, eyeWidth, image.height()).scaled(evidenceSize).save(QStringLiteral("evidence/%1-left-second.png").arg(stem));
    return check(saved, "external JPS evidence save failed")
        && check(image.text(QStringLiteral("StereoLayout")).contains(QStringLiteral("right eye first")), "external JPS layout metadata missing");
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    bool externalOk = true;
    for (int i = 1; i < argc; ++i) {
        const QString path = QString::fromLocal8Bit(argv[i]);
        externalOk = externalOk && (path.endsWith(QStringLiteral(".jps"), Qt::CaseInsensitive) ? testExternalJps(path) : testExternalMpo(path));
    }
    return testJps() && testMpo() && externalOk ? 0 : 1;
}
