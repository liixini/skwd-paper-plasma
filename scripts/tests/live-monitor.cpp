#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QDir>
#include <QTest>
#include <QPointer>
#include <functional>

// Optional KWin test. The plugin is loaded from the staged QML import directory,
// rather than linking a second copy of the monitor into this test executable.
int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (argc != 4) return 64; // PAPER FIXTURE_DIR STAGED_PLUGIN
    const QString directory = QString::fromLocal8Bit(argv[2]);
    const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR") + "/skwd-wall-v2";
    QDir().mkpath(runtime);
    auto entry = [&](const QString &output, const QString &color) {
        return QJsonObject{{"paper", QString::fromLocal8Bit(argv[1])},
            {"assignment", QJsonObject{{"outputs", QJsonArray{output}},
                {"source", QJsonObject{{"kind", "static"}, {"path", directory + "/" + color + ".png"}}}}}};
    };
    QLocalServer server;
    if (!server.listen(runtime + "/window-state.sock")) return 65;
    QPointer<QLocalSocket> second;
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (auto *peer = server.nextPendingConnection()) {
            QObject::connect(peer, &QLocalSocket::readyRead, &app, [&, peer] {
                while (peer->canReadLine()) {
                    const auto message = QJsonDocument::fromJson(peer->readLine()).object();
                    if (message["version"] != 1) continue;
                    const bool first = message["output"] == "DP-1";
                    QJsonObject policy{{"version", 1}, {"paused", first}, {"capabilities", QJsonArray{"assignments"}}};
                    if (first) policy["entry"] = entry("DP-1", "red");
                    else second = peer;
                    peer->write(QJsonDocument(policy).toJson(QJsonDocument::Compact) + '\n');
                }
            });
        }
    });
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("fixturePaper", QString::fromLocal8Bit(argv[1]));
    engine.rootContext()->setContextProperty("fallbackEntry", entry("DP-2", "blue").toVariantMap());
    engine.loadData(R"(
import QtQuick
import QtQuick.Window
import org.skwd.wallpaper 1.0
Window {
    id: root
    width: 320; height: 180; visible: true; color: "black"
    property string connector: "DP-1"
    property var current: monitor.hasEntry ? monitor.entry : (monitor.settled ? fallbackEntry : null)
    SkwdWindowMonitor { id: monitor; output: root.connector; subscribe: true }
    SkwdVideoItem {
        anchors.fill: parent; output: root.connector
        paper: fixturePaper; streamWidth: 320; streamHeight: 180
        assignment: root.current ? JSON.stringify(root.current.assignment) : ""
    }
    MouseArea { anchors.fill: parent; onClicked: root.connector = "DP-2" }
}
)");
    if (engine.rootObjects().isEmpty()) return 66;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QFile maps("/proc/self/maps");
    if (!maps.open(QIODevice::ReadOnly) || !maps.readAll().contains(QByteArray(argv[3]))) return 67;
    auto visible = [&](const QString &name, const QColor &expected) {
        QImage image;
        const bool matched = QTest::qWaitFor([&] {
            image = window->grabWindow();
            if (image.isNull()) return false;
            const QColor actual = image.pixelColor(image.width() / 2, image.height() / 2);
            return qAbs(actual.red() - expected.red()) < 8 && qAbs(actual.green() - expected.green()) < 8
                && qAbs(actual.blue() - expected.blue()) < 8;
        }, 10000);
        image.save(directory + "/monitor-" + name + ".png");
        if (!matched) qWarning("Visible monitor assignment did not become %s", qPrintable(name));
        return matched;
    };
    if (!visible("initial-red", Qt::red)) return 1;
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, QPoint(160, 90));
    if (!visible("fallback-blue", Qt::blue)) return 2;
    if (!second) return 3;
    const QJsonObject delayed{{"version", 1}, {"paused", false}, {"capabilities", QJsonArray{"assignments"}},
        {"entry", entry("DP-2", "green")}};
    second->write(QJsonDocument(delayed).toJson(QJsonDocument::Compact) + '\n');
    if (!visible("delayed-green", Qt::green)) return 4;
    qInfo("Clicked connector switch: red assignment, blue fallback, delayed green assignment verified");
    return 0;
}
