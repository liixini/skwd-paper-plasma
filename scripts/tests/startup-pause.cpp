#include "skwdvideoitem.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QFile>
#include <QDir>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>

class Item : public SkwdVideoItem {
public:
    using SkwdVideoItem::SkwdVideoItem;
    using SkwdVideoItem::componentComplete;
    using SkwdVideoItem::classBegin;
};

int main(int argc, char **argv)
{
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);
    QTemporaryDir runtime;
    if (!runtime.isValid()) return 1;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    QDir().mkpath(runtime.path() + "/skwd-paper-plasma");
    QFile helper(runtime.path() + "/presenter");
    if (!helper.open(QIODevice::WriteOnly)) return 2;
    helper.write(R"PY(#!/usr/bin/python3
import json, os, sys
with open(os.environ['XDG_RUNTIME_DIR'] + '/startup.log', 'a') as log:
    log.write(json.dumps({'paused': '--paused' in sys.argv}) + '\n')
    log.flush()
    for line in sys.stdin:
        log.write(line)
        log.flush()
)PY");
    helper.close();
    helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const auto check = [&](bool initiallyVisible, bool finallyVisible, bool explicitlyPaused) {
        QFile::remove(runtime.path() + "/startup.log");
        QQuickWindow window;
        window.setGeometry(0, 0, 32, 32);
        if (initiallyVisible) window.show();
        Item item(window.contentItem());
        item.classBegin();
        item.setSize(QSizeF(32, 32));
        item.setPaper(helper.fileName());
        item.setAssignment(QStringLiteral(R"({"source":{"kind":"video","engine":"tinier","path":"/wall/test.ivf"}})"));
        item.setPaused(explicitlyPaused);
        item.componentComplete();
        auto *process = item.findChild<QProcess *>();
        if (!process || process->state() != QProcess::Starting) {
            std::cerr << "test did not hold process in Starting" << std::endl;
            return false;
        }
        window.setVisible(finallyVisible);
        const bool expected = !finallyVisible || explicitlyPaused;
        QElapsedTimer deadline;
        deadline.start();
        while (deadline.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QFile log(runtime.path() + "/startup.log");
            if (log.open(QIODevice::ReadOnly)) {
                const auto lines = log.readAll().split('\n');
                if (lines.size() >= 3) {
                    const auto launched = QJsonDocument::fromJson(lines[0]).object();
                    const auto command = QJsonDocument::fromJson(lines[1]).object();
                    const bool ok = launched["paused"].toBool() == (!initiallyVisible || explicitlyPaused)
                        && command.contains("pause") && command["pause"].toBool() == expected;
                    if (!ok) std::cerr << "case " << initiallyVisible << finallyVisible << explicitlyPaused
                        << " launched " << lines[0].toStdString() << " command " << lines[1].toStdString() << std::endl;
                    return ok;
                }
            }
            QThread::msleep(5);
        }
        std::cerr << "latest pause state was not sent after startup" << std::endl;
        return false;
    };
    if (!check(false, true, false)) return 3;
    if (!check(true, false, false)) return 4;
    if (!check(false, true, true)) return 5;
    return 0;
}
