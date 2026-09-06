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
    QQuickWindow window;
    window.setTitle(QStringLiteral("SKWD GPU presentation test"));
    window.setMinimumSize(QSize(960, 540));
    window.setMaximumSize(QSize(960, 540));
    window.resize(960, 540);
    Item item(window.contentItem());
    item.setSize(QSizeF(960, 540));
    QObject::connect(&window, &QQuickWindow::widthChanged, &item, [&] { item.setWidth(window.width()); });
    QObject::connect(&window, &QQuickWindow::heightChanged, &item, [&] { item.setHeight(window.height()); });
    item.setStreamWidth(1920);
    item.setStreamHeight(1080);
    item.setPaper(QString::fromLocal8Bit(argv[1]));
    item.componentComplete();
    window.show();
    window.requestActivate();
    int request = 0;
    for (const auto &assignment : assignments) {
        const QString id = QStringLiteral("%1-1-%2").arg(app.applicationPid()).arg(++request);
        QFile statusFile(runtime + "/" + id + ".json");
        if (!statusFile.open(QIODevice::WriteOnly)) return 66;
        statusFile.write("{\"state\":\"pending\"}");
        statusFile.close();
        item.setPresentationId(id);
        item.setAssignment(QString::fromUtf8(QJsonDocument(assignment.toObject()).toJson(QJsonDocument::Compact)));
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
