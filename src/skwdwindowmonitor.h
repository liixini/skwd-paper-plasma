#pragma once

#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>

class SkwdWindowMonitor : public QObject {
    Q_OBJECT
    Q_PROPERTY(QAbstractItemModel *model READ model WRITE setModel NOTIFY modelChanged)
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(bool hasPolicy READ hasPolicy NOTIFY policyChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY policyChanged)

public:
    explicit SkwdWindowMonitor(QObject *parent = nullptr);
    ~SkwdWindowMonitor() override;
    QAbstractItemModel *model() const;
    void setModel(QAbstractItemModel *model);
    QString output() const;
    void setOutput(const QString &output);
    bool hasPolicy() const;
    bool paused() const;

signals:
    void modelChanged();
    void outputChanged();
    void policyChanged();

private:
    void reconnect();
    void schedule();
    void publish();
    void receivePolicy();

    QPointer<QAbstractItemModel> m_model;
    QString m_output;
    QString m_runtime;
    QString m_directory;
    QLocalSocket m_socket;
    QFileSystemWatcher m_watcher;
    QByteArray m_last;
    bool m_scheduled = false;
    bool m_hasPolicy = false;
    bool m_paused = false;
};
