#include "skwdvideoitem.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include <iostream>
#include <memory>
#include <vector>

class Item : public SkwdVideoItem {
public:
    using SkwdVideoItem::SkwdVideoItem;
    using SkwdVideoItem::componentComplete;
};

static void waitFrames(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

static bool matches(const QImage &frame, const QJsonArray &pixels, int outputs)
{
    for (int output = 0; output < outputs; ++output) {
        for (const auto &sample : pixels) {
            const auto pixel = sample.toObject();
            const QColor expected(pixel["color"].toString());
            const int x = qBound(0, int(pixel["x"].toDouble() * frame.width()), frame.width() - 1);
            const int y = qBound(0, int((output + pixel["y"].toDouble()) * frame.height() / outputs), frame.height() - 1);
            const QColor actual = frame.pixelColor(x, y);
            if (!expected.isValid() || qAbs(actual.red() - expected.red()) > 8
                || qAbs(actual.green() - expected.green()) > 8 || qAbs(actual.blue() - expected.blue()) > 8) {
                std::cerr << "output " << output << " pixel " << x << "," << y << ": expected "
                          << expected.name().toStdString() << ", got " << actual.name().toStdString() << std::endl;
                return false;
            }
        }
    }
    return true;
}

static bool changed(const QImage &first, const QImage &second, int outputs)
{
    for (int output = 0; output < outputs; ++output) {
        int different = 0, sampled = 0;
        for (int y = output * first.height() / outputs; y < (output + 1) * first.height() / outputs; y += 4) {
            for (int x = 0; x < first.width(); x += 4) {
                const QColor a = first.pixelColor(x, y), b = second.pixelColor(x, y);
                different += qAbs(a.red() - b.red()) + qAbs(a.green() - b.green()) + qAbs(a.blue() - b.blue()) > 24;
                ++sampled;
            }
        }
        if (different < sampled / 1000) return false;
    }
    return true;
}

// Optional live test: gpu-presentation-test PAPER FIXTURES_JSON ARTIFACT_DIR.
// Each fixture is {"assignment": {...}, "expect": {"pixels": [
// {"x": 0.25, "y": 0.5, "color": "#ff0000"}], "animated": true}}.
// Coordinates are fractions of each output. Animation also checks pause/resume.
// Requires a visible desktop and actual Vulkan/GL external-memory support.
int main(int argc, char **argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        std::cerr << message.toStdString() << std::endl;
    });
    QGuiApplication app(argc, argv);
    if (argc != 4) return 64;
    QFile script(QString::fromLocal8Bit(argv[2]));
    if (!script.open(QIODevice::ReadOnly)) return 65;
    const auto assignments = QJsonDocument::fromJson(script.readAll()).array();
    if (assignments.isEmpty()) return 65;
    QJsonArray expectations;
    if (qEnvironmentVariableIsSet("SKWD_TEST_EXPECTATIONS_JSON")) {
        QFile expected(qEnvironmentVariable("SKWD_TEST_EXPECTATIONS_JSON"));
        if (!expected.open(QIODevice::ReadOnly)) return 65;
        expectations = QJsonDocument::fromJson(expected.readAll()).array();
    }
    const QString artifacts = QString::fromLocal8Bit(argv[3]);
    QDir().mkpath(artifacts);
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/skwd-paper-plasma";
    QDir().mkpath(runtime);
    // SKWD_TEST_SHARED_OUTPUTS=N stacks N items with distinct output names so one
    // presenter process must serve every item.
    const int outputs = qMax(1, qEnvironmentVariableIntValue("SKWD_TEST_SHARED_OUTPUTS"));
    QQuickWindow window;
    window.setTitle(QStringLiteral("SKWD GPU presentation test"));
    const QSize size(960, 540 * outputs);
    window.setMinimumSize(size);
    window.setMaximumSize(size);
    window.resize(size);
    std::vector<std::unique_ptr<Item>> items;
    for (int index = 0; index < outputs; ++index) {
        auto item = std::make_unique<Item>(window.contentItem());
        item->setPosition(QPointF(0, 540 * index));
        item->setSize(QSizeF(960, 540));
        item->setStreamWidth(1920);
        item->setStreamHeight(1080);
        item->setPaper(QString::fromLocal8Bit(argv[1]));
        item->setOutput(QStringLiteral("TEST-%1").arg(index + 1));
        item->componentComplete();
        items.push_back(std::move(item));
    }
    Item &item = *items.front();
    window.show();
    window.requestActivate();
    int request = 0;
    for (const auto &assignment : assignments) {
        const auto fixture = assignment.toObject();
        const auto expect = fixture.contains("expect") ? fixture["expect"].toObject()
            : (request < expectations.size() ? expectations.at(request).toObject() : QJsonObject{});
        const auto pixels = expect["pixels"].toArray();
        const bool animated = expect["animated"].toBool();
        if (pixels.isEmpty()) {
            std::cerr << "Every fixture requires an assignment and explicit expected pixels" << std::endl;
            return 77; // No declared content oracle: unavailable, not a behavioral pass.
        }
        const QString id = QStringLiteral("%1-1-%2").arg(app.applicationPid()).arg(++request);
        QFile statusFile(runtime + "/" + id + ".json");
        if (!statusFile.open(QIODevice::WriteOnly)) return 66;
        statusFile.write("{\"state\":\"pending\"}");
        statusFile.close();
        const auto payload = fixture.contains("assignment") ? fixture["assignment"].toObject() : fixture;
        const QString encoded = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
        for (auto &other : items) {
            if (other.get() != &item) other->setAssignment(encoded);
        }
        item.setPresentationId(id);
        item.setAssignment(encoded);
        QElapsedTimer timer;
        timer.start();
        QJsonObject status;
        QEventLoop readyLoop;
        QTimer poll;
        QObject::connect(&poll, &QTimer::timeout, &readyLoop, [&] {
            if (statusFile.open(QIODevice::ReadOnly)) {
                status = QJsonDocument::fromJson(statusFile.readAll()).object();
                statusFile.close();
                if (status["state"] != "pending") readyLoop.quit();
            }
        });
        poll.start(10);
        QTimer::singleShot(15000, &readyLoop, &QEventLoop::quit);
        readyLoop.exec();
        poll.stop();
        std::cout << "presentation " << request << " after " << timer.elapsed() << " ms: "
                  << QJsonDocument(status).toJson(QJsonDocument::Compact).toStdString() << std::endl;
        if (status["state"] != "ready") return 1;
        {
            waitFrames(1500);
            int ready = 0;
            for (auto &other : items) ready += other->workerReady() && other->sharesWorker();
            std::cout << "shared outputs ready " << ready << "/" << outputs << std::endl;
            if (ready != outputs) return 3;
            // This process only starts Paper workers. Inspect real direct child
            // PIDs; sharesWorker() alone merely says an item joined the pool.
            QFile children(QStringLiteral("/proc/self/task/%1/children").arg(app.applicationPid()));
            if (!children.open(QIODevice::ReadOnly)) return 4;
            const auto pids = children.readAll().simplified().split(' ');
            std::cout << "live worker PIDs: " << pids.join(' ').toStdString() << std::endl;
            if (pids.size() != 1 || pids.first().isEmpty()) return 4;
        }
        QImage first;
        for (int capture = 0; capture < 2; ++capture) {
            waitFrames(750);
            const auto frame = window.grabWindow();
            if (frame.isNull() || !frame.save(artifacts + QStringLiteral("/%1-%2.png").arg(request).arg(capture))) return 2;
            if (!matches(frame, pixels, outputs)) return 5;
            if (capture == 0) first = frame;
            else if (animated && !changed(first, frame, outputs)) return 6;
        }
        if (animated) {
            for (auto &other : items) other->setPaused(true);
            waitFrames(1000);
            const QImage paused = window.grabWindow();
            paused.save(artifacts + QStringLiteral("/%1-paused.png").arg(request));
            waitFrames(750);
            const QImage held = window.grabWindow();
            held.save(artifacts + QStringLiteral("/%1-held.png").arg(request));
            if (paused != held) { std::cerr << "paused frames changed" << std::endl; return 7; }
            for (auto &other : items) other->setPaused(false);
            waitFrames(750);
            const QImage resumed = window.grabWindow();
            resumed.save(artifacts + QStringLiteral("/%1-resumed.png").arg(request));
            if (!changed(held, resumed, outputs)) return 8;
        }
        statusFile.remove();
    }
    return 0;
}
