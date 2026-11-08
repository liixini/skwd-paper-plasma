#include "../../src/skwdvideoitem.cpp"

#include <QGuiApplication>
#include <QDir>
#include <QQuickWindow>
#include <QThread>
#include <cstdio>

class ProbeItem : public QQuickItem {
public:
    explicit ProbeItem(QQuickItem *parent) : QQuickItem(parent) { setFlag(ItemHasContents); }

    std::shared_ptr<SkwdWidgetSnapshot> snapshot;
    quint64 generation = 1;

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override
    {
        auto *node = static_cast<SkwdFrameNode *>(old);
        if (!node) {
            node = new SkwdFrameNode;
            QImage base(16, 16, QImage::Format_RGBA8888);
            base.fill(Qt::green);
            node->setTexture(window()->createTextureFromImage(base, QQuickWindow::TextureIsOpaque));
            node->setOwnsTexture(true);
            node->streamGeneration = generation;
        }
        node->streamGeneration = generation;
        node->setRect(boundingRect());
        if (!node->showWidgets(snapshot, window(), boundingRect())) qFatal("widget texture upload failed");
        return node;
    }
};

static std::shared_ptr<SkwdWidgetSnapshot> snapshot(quint64 revision)
{
    constexpr size_t LayerBytes = 24;
    constexpr size_t CanvasBytes = 4 * 4 * 4;
    constexpr size_t Length = 2 * LayerBytes + 2 * CanvasBytes;
    std::array<uchar, Length> bytes {};
    qToLittleEndian(quint32(4), bytes.data() + 8);
    qToLittleEndian(quint32(4), bytes.data() + 12);
    qToLittleEndian(quint64(2 * LayerBytes), bytes.data() + 16);
    qToLittleEndian(quint32(2), bytes.data() + LayerBytes);
    qToLittleEndian(quint32(4), bytes.data() + LayerBytes + 8);
    qToLittleEndian(quint32(4), bytes.data() + LayerBytes + 12);
    qToLittleEndian(quint64(2 * LayerBytes + CanvasBytes), bytes.data() + LayerBytes + 16);
    for (size_t index = 2 * LayerBytes; index < 2 * LayerBytes + CanvasBytes; index += 4)
        std::memcpy(bytes.data() + index, "\x80\x00\x00\x80", 4);
    for (size_t index = 2 * LayerBytes + CanvasBytes; index < Length; index += 4)
        std::memcpy(bytes.data() + index, "\x00\x00\x80\x80", 4);
    const int fd = ::memfd_create("skwd-widget-render", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ::write(fd, bytes.data(), bytes.size()) != ssize_t(bytes.size())
        || ::fcntl(fd, F_ADD_SEALS, WidgetSeals) < 0) qFatal("widget backing failed");
    void *pixels = ::mmap(nullptr, bytes.size(), PROT_READ, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) qFatal("widget map failed");
    auto result = std::make_shared<SkwdWidgetSnapshot>();
    result->fd = fd;
    result->pixels = pixels;
    result->length = bytes.size();
    result->revision = revision;
    result->target = QSize(16, 16);
    result->layers = {{QRectF(0, 0, 4, 4), 4, 4, 2 * LayerBytes},
                      {QRectF(2, 0, 4, 4), 4, 4, 2 * LayerBytes + CanvasBytes}};
    return result;
}

static QImage capture(QQuickWindow &window)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
    return window.grabWindow();
}

static bool near(const QColor &actual, const QColor &expected)
{
    return qAbs(actual.red() - expected.red()) < 8
        && qAbs(actual.green() - expected.green()) < 8
        && qAbs(actual.blue() - expected.blue()) < 8;
}

static void save(const QImage &image, const QString &name)
{
    const QString path = qEnvironmentVariable("SKWD_TEST_ARTIFACT_DIR");
    if (path.isEmpty()) return;
    QDir().mkpath(path);
    image.save(path + QLatin1Char('/') + name + QStringLiteral(".png"));
}

int main(int argc, char **argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    QQuickWindow window;
    window.resize(32, 32);
    ProbeItem item(window.contentItem());
    item.setSize(QSizeF(32, 32));
    item.snapshot = snapshot(1);
    window.show();
    QImage first = capture(window);
    save(first, QStringLiteral("widget-alpha"));
    if (first.isNull() || !near(first.pixelColor(2, 2), QColor(128, 127, 0))
        || !near(first.pixelColor(6, 2), QColor(64, 63, 128))
        || !near(first.pixelColor(10, 2), QColor(0, 127, 128))
        || first.pixelColor(16, 16) != Qt::green) {
        std::printf("alpha %s %s %s %s\n", qPrintable(first.pixelColor(2, 2).name()),
            qPrintable(first.pixelColor(6, 2).name()), qPrintable(first.pixelColor(10, 2).name()),
            qPrintable(first.pixelColor(16, 16).name()));
        return 1;
    }
    window.resize(64, 64);
    item.setSize(QSizeF(64, 64));
    item.update();
    QImage scaled = capture(window);
    save(scaled, QStringLiteral("widget-scaled"));
    if (scaled.isNull() || !near(scaled.pixelColor(12, 4), QColor(64, 63, 128))
        || scaled.pixelColor(32, 32) != Qt::green) return 4;
    item.snapshot.reset();
    item.update();
    QImage clear = capture(window);
    save(clear, QStringLiteral("widget-clear"));
    if (clear.isNull() || clear.pixelColor(12, 4) != Qt::green) return 2;
    item.generation = 2;
    item.snapshot = snapshot(1);
    item.update();
    QImage replaced = capture(window);
    save(replaced, QStringLiteral("widget-replaced"));
    if (replaced.isNull() || !near(replaced.pixelColor(12, 4), QColor(64, 63, 128))) return 3;
    return 0;
}
