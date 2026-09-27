// Capture actual KWin-composed output pixels for the private activities test.
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QImage>
#include <cstdio>
#include <unistd.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.size() != 3) return 2;
    QDBusInterface iface("org.kde.KWin.ScreenShot2", "/org/kde/KWin/ScreenShot2", "org.kde.KWin.ScreenShot2");
    iface.setTimeout(10000);
    int fd[2];
    if (pipe(fd)) return 3;
    QDBusReply<QVariantMap> reply = iface.call("CaptureScreen", args[1],
        QVariantMap{{"hide-caller-windows", false}}, QVariant::fromValue(QDBusUnixFileDescriptor(fd[1])));
    close(fd[1]);
    if (!reply.isValid()) {
        fprintf(stderr, "%s\n", qPrintable(reply.error().message()));
        close(fd[0]);
        return 4;
    }
    const auto metadata = reply.value();
    const int width = metadata.value("width").toInt();
    const int height = metadata.value("height").toInt();
    const int stride = metadata.value("stride").toInt();
    if (width <= 0 || height <= 0 || stride < width * 4) return 5;
    QByteArray pixels(qsizetype(stride) * height, '\0');
    qsizetype count = 0;
    while (count < pixels.size()) {
        const auto n = read(fd[0], pixels.data() + count, pixels.size() - count);
        if (n <= 0) break;
        count += n;
    }
    close(fd[0]);
    if (count != pixels.size()) return 6;
    QImage image(reinterpret_cast<const uchar *>(pixels.constData()), width, height, stride,
                 QImage::Format(metadata.value("format").toInt()));
    return image.save(args[2]) ? 0 : 7;
}
