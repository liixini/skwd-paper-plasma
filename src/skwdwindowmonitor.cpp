#include "skwdwindowmonitor.h"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTimer>

SkwdWindowMonitor::SkwdWindowMonitor(QObject *parent)
    : QObject(parent)
    , m_runtime(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
    , m_directory(m_runtime + QStringLiteral("/skwd-wall-v2"))
{
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &SkwdWindowMonitor::reconnect);
    connect(&m_socket, &QLocalSocket::connected, this, [this] { m_last.clear(); schedule(); });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        m_hasPolicy = false;
        emit policyChanged();
        reconnect();
    });
    connect(&m_socket, &QLocalSocket::bytesWritten, this, &SkwdWindowMonitor::schedule);
    connect(&m_socket, &QLocalSocket::readyRead, this, &SkwdWindowMonitor::receivePolicy);
    m_socket.setReadBufferSize(4097);
    reconnect();
}

QAbstractItemModel *SkwdWindowMonitor::model() const { return m_model; }
SkwdWindowMonitor::~SkwdWindowMonitor()
{
    disconnect(&m_socket, nullptr, this, nullptr);
    m_socket.abort();
}
QString SkwdWindowMonitor::output() const { return m_output; }
bool SkwdWindowMonitor::hasPolicy() const { return m_hasPolicy; }
bool SkwdWindowMonitor::paused() const { return m_paused; }

void SkwdWindowMonitor::receivePolicy()
{
    while (m_socket.canReadLine()) {
        const QByteArray line = m_socket.readLine(4097);
        const auto value = QJsonDocument::fromJson(line).object();
        if (!line.endsWith('\n') || value.value("version").toInt() != 1 || !value.value("paused").isBool()) {
            m_socket.abort();
            return;
        }
        m_hasPolicy = true;
        m_paused = value.value("paused").toBool();
        emit policyChanged();
    }
    if (m_socket.bytesAvailable() >= 4097) m_socket.abort();
}

void SkwdWindowMonitor::setModel(QAbstractItemModel *model)
{
    if (m_model == model) return;
    if (m_model) disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    if (model) {
        connect(model, &QAbstractItemModel::dataChanged, this, &SkwdWindowMonitor::schedule);
        connect(model, &QAbstractItemModel::rowsInserted, this, &SkwdWindowMonitor::schedule);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &SkwdWindowMonitor::schedule);
        connect(model, &QAbstractItemModel::rowsMoved, this, &SkwdWindowMonitor::schedule);
        connect(model, &QAbstractItemModel::modelReset, this, &SkwdWindowMonitor::schedule);
        connect(model, &QAbstractItemModel::layoutChanged, this, &SkwdWindowMonitor::schedule);
        connect(model, &QObject::destroyed, this, &SkwdWindowMonitor::schedule);
    }
    emit modelChanged();
    schedule();
}

void SkwdWindowMonitor::setOutput(const QString &output)
{
    if (m_output == output) return;
    m_output = output;
    emit outputChanged();
    schedule();
}

void SkwdWindowMonitor::reconnect()
{
    for (const QString &path : {m_runtime, m_directory}) {
        if (QDir(path).exists() && !m_watcher.directories().contains(path)) m_watcher.addPath(path);
    }
    if (!m_runtime.isEmpty() && m_socket.state() == QLocalSocket::UnconnectedState) {
        m_socket.connectToServer(m_directory + QStringLiteral("/window-state.sock"));
    }
}

void SkwdWindowMonitor::schedule()
{
    if (m_scheduled) return;
    m_scheduled = true;
    QTimer::singleShot(0, this, [this] { m_scheduled = false; publish(); });
}

void SkwdWindowMonitor::publish()
{
    if (m_socket.state() != QLocalSocket::ConnectedState || m_socket.bytesToWrite() != 0 || m_output.isEmpty()) return;
    bool supported = false;
    bool fullscreen = false;
    bool maximized = false;
    if (m_model) {
        const auto roles = m_model->roleNames();
        const int windowRole = roles.key("IsWindow", -1);
        const int fullscreenRole = roles.key("IsFullScreen", -1);
        const int maximizedRole = roles.key("IsMaximized", -1);
        const int minimizedRole = roles.key("IsMinimized", -1);
        supported = windowRole >= 0 && fullscreenRole >= 0 && maximizedRole >= 0 && minimizedRole >= 0;
        if (supported) {
            for (int row = 0; row < m_model->rowCount(); ++row) {
                const QModelIndex index = m_model->index(row, 0);
                if (!m_model->data(index, windowRole).toBool() || m_model->data(index, minimizedRole).toBool()) continue;
                fullscreen |= m_model->data(index, fullscreenRole).toBool();
                maximized |= m_model->data(index, maximizedRole).toBool();
            }
        }
    }
    const QByteArray message = QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1}, {QStringLiteral("output"), m_output},
        {QStringLiteral("supported"), supported}, {QStringLiteral("fullscreen"), fullscreen},
        {QStringLiteral("maximized"), maximized}
    }).toJson(QJsonDocument::Compact) + '\n';
    if (message != m_last && m_socket.write(message) == message.size()) m_last = message;
}
