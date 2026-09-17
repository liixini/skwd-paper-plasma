#pragma once

#include <QAbstractItemModel>
#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>

class SkwdWindowMonitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(bool hasPolicy READ hasPolicy NOTIFY policyChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY policyChanged)
    Q_PROPERTY(bool subscribe READ subscribe WRITE setSubscribe NOTIFY subscribeChanged)
    Q_PROPERTY(QVariantMap entry READ entry NOTIFY entryChanged)
    Q_PROPERTY(bool hasEntry READ hasEntry NOTIFY entryChanged)
    Q_PROPERTY(bool settled READ settled NOTIFY settledChanged)

public:
    explicit SkwdWindowMonitor(QObject *parent = nullptr);
    ~SkwdWindowMonitor() override;
    QAbstractItemModel *model() const;
    void setModel(QAbstractItemModel *model);
    QString output() const;
    void setOutput(const QString &output);
    bool hasPolicy() const;
    bool paused() const;
    bool subscribe() const;
    void setSubscribe(bool subscribe);
    QVariantMap entry() const;
    bool hasEntry() const;
    bool settled() const;

signals:
    void modelChanged();
    void outputChanged();
    void policyChanged();
    void subscribeChanged();
    void entryChanged();
    void settledChanged();

private:
    void reconnect();
    void lost();
    void watch();
    void unwatch();
    void schedule();
    void publish();
    void receivePolicy();
    void requestAssignments();
    void settle();

    QPointer<QAbstractItemModel> m_model;
    QString m_output;
    QString m_runtime;
    QString m_directory;
    QLocalSocket m_socket;
    QFileSystemWatcher m_watcher;
    QTimer m_retry;
    QTimer m_grace;
    QVariantMap m_entry;
    QElapsedTimer m_connectedAt;
    QByteArray m_last;
    int m_failures = 0;
    bool m_scheduled = false;
    bool m_hasPolicy = false;
    bool m_paused = false;
    bool m_subscribe = false;
    bool m_capable = false;
    bool m_subscribed = false;
    bool m_hasEntry = false;
    bool m_settled = false;
};
