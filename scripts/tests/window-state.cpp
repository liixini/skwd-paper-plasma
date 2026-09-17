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

static void waitFor(const std::function<bool()> &condition, qint64 timeout = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) {
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

static void observations() {
    QTemporaryDir workspace;
    if (!workspace.isValid()) qFatal("Cannot create a runtime directory");
    qputenv("XDG_RUNTIME_DIR", workspace.path().toUtf8());
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
}

struct Session {
    QTemporaryDir workspace;
    Tasks tasks;
    std::unique_ptr<SkwdWindowMonitor> monitor;
    QLocalServer server;
    std::unique_ptr<QLocalSocket> socket;

    Session() {
        if (!workspace.isValid()) qFatal("Cannot create a runtime directory");
        qputenv("XDG_RUNTIME_DIR", workspace.path().toUtf8());
        QDir().mkpath(workspace.path() + "/skwd-wall-v2");
        if (!server.listen(workspace.path() + "/skwd-wall-v2/window-state.sock")) qFatal("Cannot listen for window states");
        tasks.rows = {{true, false, false}};
        monitor = std::make_unique<SkwdWindowMonitor>();
        monitor->setModel(&tasks);
        monitor->setOutput("DP-1");
        accept(3000);
        expect(socket.get(), true, true, false);
    }

    void accept(qint64 timeout) {
        waitFor([&] { return server.hasPendingConnections(); }, timeout);
        socket.reset(server.nextPendingConnection());
    }
};

static void peerDropWhileListening() {
    Session session;
    for (int round = 0; round < 4; ++round) {
        session.socket.reset();
        session.accept(10000);
        const bool fullscreen = round % 2 == 0;
        expect(session.socket.get(), true, fullscreen, !fullscreen);
        session.tasks.replace({{!fullscreen, fullscreen, false}});
        expect(session.socket.get(), true, !fullscreen, fullscreen);
    }
}

static void rejectingPeerIsBounded() {
    Session session;
    session.socket.reset();
    int attempts = 0;
    QElapsedTimer window;
    window.start();
    while (window.elapsed() < 2000) {
        QCoreApplication::processEvents();
        while (session.server.hasPendingConnections()) {
            delete session.server.nextPendingConnection();
            ++attempts;
        }
        QThread::msleep(1);
    }
    if (attempts < 2 || attempts > 6) qFatal("Window-state reconnects were not bounded: %d attempts in 2s", attempts);
    session.accept(10000);
    expect(session.socket.get(), true, true, false);
}

static QJsonObject request(QLocalSocket *socket) {
    waitFor([&] { return socket->canReadLine(); });
    return QJsonDocument::fromJson(socket->readLine()).object();
}

static const QJsonObject subscription{
    {"version", 2}, {"output", "DP-1"}, {"subscribe", "assignments"}
};

static void pushedAssignmentsSurviveReconnects() {
    Session session;
    session.monitor->setSubscribe(true);
    if (session.monitor->settled()) qFatal("A subscribing monitor settled before the daemon answered");
    session.socket->write("{\"version\":1,\"paused\":false,\"capabilities\":[\"assignments\"]}\n");
    session.socket->flush();
    if (request(session.socket.get()) != subscription) qFatal("Monitor did not subscribe to assignments");
    session.socket->write("{\"version\":1,\"paused\":false,\"capabilities\":[\"assignments\"],"
                          "\"entry\":{\"paper\":\"/paper\",\"assignment\":{\"outputs\":[\"DP-1\"]}}}\n");
    session.socket->flush();
    waitFor([&] { return session.monitor->hasEntry(); });
    if (session.monitor->entry().value("paper").toString() != "/paper") qFatal("Pushed entry was not exposed");
    session.socket.reset();
    session.accept(10000);
    expect(session.socket.get(), true, true, false);
    if (!session.monitor->hasEntry()) qFatal("Reconnecting dropped the last pushed entry");
    session.socket->write("{\"version\":1,\"paused\":false,\"capabilities\":[\"assignments\"]}\n");
    session.socket->flush();
    if (request(session.socket.get()) != subscription) qFatal("Monitor did not resubscribe after reconnecting");
    session.socket.reset();
    session.accept(10000);
    expect(session.socket.get(), true, true, false);
    session.socket->write("{\"version\":1,\"paused\":false}\n");
    session.socket->flush();
    waitFor([&] { return !session.monitor->hasEntry() && session.monitor->settled(); });
}

static void olderDaemonKeepsConfiguredAssignments() {
    Session session;
    session.monitor->setSubscribe(true);
    session.socket->write("{\"version\":1,\"paused\":false}\n");
    session.socket->flush();
    waitFor([&] { return session.monitor->settled(); });
    QElapsedTimer quiet;
    quiet.start();
    while (quiet.elapsed() < 150) QCoreApplication::processEvents();
    if (session.socket->canReadLine() || session.monitor->hasEntry()) qFatal("Monitor subscribed to a daemon without assignments");
}

static void missingDaemonSettlesAfterGrace() {
    QTemporaryDir workspace;
    if (!workspace.isValid()) qFatal("Cannot create a runtime directory");
    qputenv("XDG_RUNTIME_DIR", workspace.path().toUtf8());
    SkwdWindowMonitor monitor;
    monitor.setOutput("DP-1");
    monitor.setSubscribe(true);
    if (monitor.settled()) qFatal("Monitor settled without waiting for the daemon");
    waitFor([&] { return monitor.settled(); }, 5000);
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    observations();
    peerDropWhileListening();
    rejectingPeerIsBounded();
    pushedAssignmentsSurviveReconnects();
    olderDaemonKeepsConfiguredAssignments();
    missingDaemonSettlesAfterGrace();
    return 0;
}
