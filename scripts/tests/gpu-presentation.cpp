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
#include <iostream>
#include <memory>
#include <vector>

class Item : public SkwdVideoItem {
public:
    using SkwdVideoItem::SkwdVideoItem;
    using SkwdVideoItem::componentComplete;
};

// Optional live test: gpu-presentation-test PAPER ASSIGNMENTS_JSON ARTIFACT_DIR.
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
        if (outputs > 1) {
            item->setOutput(QStringLiteral("TEST-%1").arg(index + 1));
        }
        item->componentComplete();
        items.push_back(std::move(item));
    }
    Item &item = *items.front();
    window.show();
    window.requestActivate();
    int request = 0;
    for (const auto &assignment : assignments) {
        const QString id = QStringLiteral("%1-1-%2").arg(app.applicationPid()).arg(++request);
        QFile statusFile(runtime + "/" + id + ".json");
        if (!statusFile.open(QIODevice::WriteOnly)) return 66;
        statusFile.write("{\"state\":\"pending\"}");
        statusFile.close();
        const QString encoded = QString::fromUtf8(QJsonDocument(assignment.toObject()).toJson(QJsonDocument::Compact));
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
        if (outputs > 1) {
            QEventLoop settle;
            QTimer::singleShot(1500, &settle, &QEventLoop::quit);
            settle.exec();
            int ready = 0;
            for (auto &other : items) ready += other->workerReady() && other->sharesWorker();
            std::cout << "shared outputs ready " << ready << "/" << outputs << std::endl;
            if (ready != outputs) return 3;
        }
        for (int capture = 0; capture < 2; ++capture) {
            QEventLoop frameLoop;
            QTimer::singleShot(750, &frameLoop, &QEventLoop::quit);
            frameLoop.exec();
            const auto frame = window.grabWindow();
            if (frame.isNull() || !frame.save(artifacts + QStringLiteral("/%1-%2.png").arg(request).arg(capture))) return 2;
        }
        statusFile.remove();
    }
    return 0;
}
