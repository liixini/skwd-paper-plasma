#include "skwdvideoitem.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

class Item : public SkwdVideoItem {
public:
    using SkwdVideoItem::SkwdVideoItem;
    using SkwdVideoItem::componentComplete;
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    QQuickWindow::setSceneGraphBackend("software");
    QGuiApplication app(argc, argv);
    QQuickWindow window;
    window.resize(32, 32);
    QDir().mkpath(runtime.path() + "/skwd-paper-plasma");
    QFile helper(runtime.path() + "/presenter");
    if (!helper.open(QIODevice::WriteOnly)) return 1;
    helper.write(R"PY(#!/usr/bin/python3
import os, socket, struct, sys
mode = sys.argv[sys.argv.index('--assignment') + 1]
if mode == 'fail':
    sys.stderr.write('synthetic export unavailable\n')
    sys.exit(2)
if mode == 'ready':
    socket.socket(fileno=3).send(b'SKDG' + bytes([6]) + bytes(27))
sys.stdout.buffer.write(b'SKWP' + struct.pack('<II',16,16) + bytes([255,0,0,255])*256)
sys.stdout.buffer.flush()
if mode == 'prelude':
    sys.stderr.write('failed after transition prelude\n')
    sys.exit(3)
sys.stdin.read()
)PY");
    helper.close();
    helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Item item(window.contentItem());
    item.setSize(QSizeF(32,32));
    item.setPaper(helper.fileName());
    item.componentComplete();
    window.show();
    int request = 0;
    auto check = [&](const QString &assignment, const QString &expected, const QString &detail = QString()) {
        const QString id = QStringLiteral("123-1-%1").arg(++request);
        QFile file(runtime.path() + "/skwd-paper-plasma/" + id + ".json");
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write("{\"state\":\"pending\"}");
        file.close();
        item.setPresentationId(id);
        item.setAssignment(assignment);
        QElapsedTimer timer;
        timer.start();
        QJsonObject status;
        while (timer.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (file.open(QIODevice::ReadOnly)) {
                status = QJsonDocument::fromJson(file.readAll()).object();
                file.close();
                if (status["state"] == expected) break;
            }
            QThread::msleep(5);
        }
        const bool ok = status["state"] == expected && status["error"].toString().contains(detail);
        if (!ok) std::cerr << "presentation " << assignment.toStdString() << ": " << QJsonDocument(status).toJson().toStdString();
        return ok;
    };
    if (!check("ready", "ready")) return 2;
    if (!check("ready", "ready")) return 3;
    if (!check("fail", "error", "synthetic export unavailable")) return 4;
    if (!check("prelude", "error", "failed after transition prelude")) return 5;
    item.setPaper(runtime.path() + "/missing");
    if (!check("missing", "error", "Cannot start")) return 6;
    return 0;
}
