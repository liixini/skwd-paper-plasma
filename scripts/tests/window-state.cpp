#include "skwdwindowmonitor.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <memory>

class Tasks : public QAbstractListModel {
public:
    struct Row { bool fullscreen; bool maximized; bool minimized; };
    QList<Row> rows;
    bool valid = true;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : rows.size(); }
    QHash<int, QByteArray> roleNames() const override {
        if (!valid) return {};
        return {{Qt::UserRole, "IsWindow"}, {Qt::UserRole + 1, "IsFullScreen"},
                {Qt::UserRole + 2, "IsMaximized"}, {Qt::UserRole + 3, "IsMinimized"}};
    }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= rows.size()) return {};
        const auto row = rows.at(index.row());
        switch (role - Qt::UserRole) {
        case 0: return true;
        case 1: return row.fullscreen;
        case 2: return row.maximized;
        case 3: return row.minimized;
        default: return {};
        }
    }
    void replace(QList<Row> value) { beginResetModel(); rows = value; endResetModel(); }
};

static void waitFor(const std::function<bool()> &condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    if (!condition()) qFatal("Window-state condition timed out");
}

static void expect(QLocalSocket *socket, bool supported, bool fullscreen, bool maximized) {
    waitFor([&] { return socket->canReadLine(); });
    const auto value = QJsonDocument::fromJson(socket->readLine()).object();
    if (value.value("version").toInt() != 1 || value.value("output").toString() != "DP-1"
        || value.value("supported").toBool() != supported
        || value.value("fullscreen").toBool() != fullscreen
        || value.value("maximized").toBool() != maximized) {
        qFatal("Unexpected window-state observation: %s", qPrintable(QString::fromUtf8(QJsonDocument(value).toJson())));
    }
}

int main(int argc, char **argv) {
    QTemporaryDir workspace;
    if (!workspace.isValid()) return 1;
    qputenv("XDG_RUNTIME_DIR", workspace.path().toUtf8());
    QCoreApplication application(argc, argv);
    const QString directory = workspace.path() + "/skwd-wall-v2";
    const QString path = directory + "/window-state.sock";
    Tasks tasks;
    SkwdWindowMonitor monitor;
    monitor.setModel(&tasks);
    monitor.setOutput("DP-1");
    QCoreApplication::processEvents();
    QDir().mkpath(directory);
    QLocalServer server;
    if (!server.listen(path)) qFatal("Cannot listen for window states");
    waitFor([&] { return server.hasPendingConnections(); });
    std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
    expect(socket.get(), true, false, false);
    socket->write("{\"version\":1,\"paused\":true}\n");
    socket->flush();
    waitFor([&] { return monitor.hasPolicy() && monitor.paused(); });
    socket->write("{\"version\":1,\"paused\":false}\n");
    socket->flush();
    waitFor([&] { return monitor.hasPolicy() && !monitor.paused(); });
    tasks.replace({{true, false, false}, {false, true, false}});
    expect(socket.get(), true, true, true);
    tasks.replace({{true, false, true}, {false, true, false}});
    expect(socket.get(), true, false, true);
    tasks.replace({});
    expect(socket.get(), true, false, false);
    tasks.valid = false;
    tasks.replace({{true, true, false}});
    expect(socket.get(), false, false, false);
    tasks.valid = true;
    tasks.replace({{true, false, false}});
    expect(socket.get(), true, true, false);
    socket.reset();
    server.close();
    waitFor([&] { return !monitor.hasPolicy(); });
    if (!server.listen(path)) qFatal("Cannot restart window-state listener");
    waitFor([&] { return server.hasPendingConnections(); });
    socket.reset(server.nextPendingConnection());
    expect(socket.get(), true, true, false);
    monitor.setModel(nullptr);
    expect(socket.get(), false, false, false);
    return 0;
}
