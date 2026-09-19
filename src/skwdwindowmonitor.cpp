#include "skwdwindowmonitor.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace {
constexpr qint64 BriefConnectionMs = 1000;
constexpr int RetryBaseMs = 250;
constexpr int RetryLimit = 6;
constexpr int GraceMs = 2000;
constexpr qint64 LineLimit = 1024 * 1024;
}

SkwdWindowMonitor::SkwdWindowMonitor(QObject *parent)
    : QObject(parent)
    , m_runtime(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation))
    , m_directory(m_runtime + QStringLiteral("/skwd-wall-v2"))
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &SkwdWindowMonitor::reconnect);
    m_grace.setSingleShot(true);
    connect(&m_grace, &QTimer::timeout, this, &SkwdWindowMonitor::settle);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (!m_retry.isActive()) m_retry.start(0);
    });
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_connectedAt.start();
        unwatch();
        m_last.clear();
        schedule();
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, &SkwdWindowMonitor::lost);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] {
        if (m_socket.state() == QLocalSocket::UnconnectedState) watch();
    });
    connect(&m_socket, &QLocalSocket::bytesWritten, this, &SkwdWindowMonitor::schedule);
    connect(&m_socket, &QLocalSocket::readyRead, this, &SkwdWindowMonitor::receivePolicy);
    m_socket.setReadBufferSize(LineLimit + 1);
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
bool SkwdWindowMonitor::subscribe() const { return m_subscribe; }
QVariantMap SkwdWindowMonitor::entry() const { return m_entry; }
bool SkwdWindowMonitor::hasEntry() const { return m_hasEntry; }
bool SkwdWindowMonitor::settled() const { return !m_subscribe || m_settled; }

void SkwdWindowMonitor::setSubscribe(bool subscribe)
{
    if (m_subscribe == subscribe) return;
    m_subscribe = subscribe;
    emit subscribeChanged();
    emit settledChanged();
    if (subscribe && !m_settled && !m_hasEntry) m_grace.start(GraceMs);
    requestAssignments();
}

void SkwdWindowMonitor::requestAssignments()
{
    if (!m_subscribe || !m_capable || m_subscribed || m_output.isEmpty()
        || m_socket.state() != QLocalSocket::ConnectedState) {
        return;
    }
    const QByteArray line = QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 2}, {QStringLiteral("output"), m_output},
        {QStringLiteral("subscribe"), QStringLiteral("assignments")}
    }).toJson(QJsonDocument::Compact) + '\n';
    m_subscribed = m_socket.write(line) == line.size();
}

void SkwdWindowMonitor::settle()
{
    if (m_settled) return;
    m_settled = true;
    m_grace.stop();
    emit settledChanged();
}

void SkwdWindowMonitor::receivePolicy()
{
    while (m_socket.canReadLine()) {
        const QByteArray line = m_socket.readLine(LineLimit + 1);
        const auto value = QJsonDocument::fromJson(line).object();
        if (!line.endsWith('\n') || value.value("version").toInt() != 1 || !value.value("paused").isBool()) {
            m_socket.abort();
            return;
        }
        m_hasPolicy = true;
        m_paused = value.value("paused").toBool();
        emit policyChanged();
        if (!value.value("capabilities").toArray().contains(QStringLiteral("assignments"))) {
            settle();
            if (m_hasEntry) {
                m_hasEntry = false;
                m_entry.clear();
                emit entryChanged();
            }
        } else if (!m_capable) {
            m_capable = true;
            requestAssignments();
        }
        const auto entry = value.value("entry");
        if (entry.isObject() && (!m_hasEntry || entry.toObject().toVariantMap() != m_entry)) {
            m_entry = entry.toObject().toVariantMap();
            m_hasEntry = true;
            m_grace.stop();
            emit entryChanged();
        }
    }
    if (m_socket.bytesAvailable() > LineLimit) m_socket.abort();
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
    const bool resubscribe = m_subscribed;
    m_output = output;
    // Cached assignments survive a reconnect only for the same connector.
    // A new connector gets its own subscription and fallback grace period.
    if (m_hasEntry) {
        m_hasEntry = false;
        m_entry.clear();
        emit entryChanged();
    }
    m_hasPolicy = false;
    m_paused = false;
    emit policyChanged();
    if (m_settled) {
        m_settled = false;
        emit settledChanged();
    }
    if (m_subscribe) m_grace.start(GraceMs);
    emit outputChanged();
    if (resubscribe) {
        m_socket.abort();
        return;
    }
    schedule();
    requestAssignments();
}

void SkwdWindowMonitor::reconnect()
{
    if (m_runtime.isEmpty() || m_socket.state() != QLocalSocket::UnconnectedState) return;
    watch();
    const QString path = m_directory + QStringLiteral("/window-state.sock");
    if (QFileInfo::exists(path)) m_socket.connectToServer(path);
}

void SkwdWindowMonitor::lost()
{
    m_capable = false;
    m_subscribed = false;
    m_hasPolicy = false;
    emit policyChanged();
    const bool brief = !m_connectedAt.isValid() || m_connectedAt.elapsed() < BriefConnectionMs;
    m_connectedAt.invalidate();
    m_failures = brief ? m_failures + 1 : 0;
    watch();
    if (m_failures <= RetryLimit) m_retry.start(m_failures == 0 ? 0 : RetryBaseMs << (m_failures - 1));
}

void SkwdWindowMonitor::watch()
{
    if (m_runtime.isEmpty()) return;
    const QString target = QFileInfo(m_directory).isDir() ? m_directory : m_runtime;
    const QStringList watched = m_watcher.directories();
    if (watched.size() == 1 && watched.first() == target) return;
    if (!watched.isEmpty()) m_watcher.removePaths(watched);
    m_watcher.addPath(target);
}

void SkwdWindowMonitor::unwatch()
{
    const QStringList watched = m_watcher.directories();
    if (!watched.isEmpty()) m_watcher.removePaths(watched);
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
