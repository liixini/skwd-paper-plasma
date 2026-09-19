#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QSet>

class SkwdVideoItem;

class SkwdWorkerPool final : public QObject {
    Q_OBJECT

public:
    static SkwdWorkerPool *instance();

    void update(SkwdVideoItem *item);
    bool retune(SkwdVideoItem *item, const QByteArray &line);
    void detach(SkwdVideoItem *item);
    bool sendControl(SkwdVideoItem *item, const QByteArray &line);
    int workerCount() const;
    bool legacyPresenter(const QString &paper) const;

private:
    struct Worker {
        QByteArray key;
        QProcess *process = nullptr;
        QList<SkwdVideoItem *> members;
        QByteArray errors;
        bool spawnScheduled = false;
        bool stopping = false;
    };

    explicit SkwdWorkerPool(QObject *parent = nullptr);
    Worker *workerFor(const QByteArray &key);
    void createProcess(Worker *worker);
    void scheduleSpawn(Worker *worker);
    void spawn(Worker *worker);
    void stop(Worker *worker);
    void release(Worker *worker);
    void finished(Worker *worker, int code, QProcess::ExitStatus status);

    QHash<QByteArray, Worker *> m_workers;
    QHash<SkwdVideoItem *, Worker *> m_membership;
    QSet<QString> m_legacyPapers;
};
