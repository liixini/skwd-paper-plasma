#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QProcess>
#include <QQuickItem>
#include <array>
#include <vector>

class QSocketNotifier;
class SkwdWorkerPool;

class SkwdVideoItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString presentationId READ presentationId WRITE setPresentationId NOTIFY presentationIdChanged)
    Q_PROPERTY(QString assignment READ assignment WRITE setAssignment NOTIFY assignmentChanged)
    Q_PROPERTY(QString paper READ paper WRITE setPaper NOTIFY paperChanged)
    Q_PROPERTY(int streamWidth READ streamWidth WRITE setStreamWidth NOTIFY streamSizeChanged)
    Q_PROPERTY(int streamHeight READ streamHeight WRITE setStreamHeight NOTIFY streamSizeChanged)
    Q_PROPERTY(int streamFps READ streamFps WRITE setStreamFps NOTIFY streamFpsChanged)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY pausedChanged)
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)

public:
    explicit SkwdVideoItem(QQuickItem *parent = nullptr);
    ~SkwdVideoItem() override;

    QString presentationId() const;
    void setPresentationId(const QString &value);
    QString assignment() const;
    void setAssignment(const QString &value);
    QString paper() const;
    void setPaper(const QString &value);
    int streamWidth() const;
    void setStreamWidth(int value);
    int streamHeight() const;
    void setStreamHeight(int value);
    int streamFps() const;
    void setStreamFps(int value);
    bool paused() const;
    void setPaused(bool value);
    QString output() const;
    void setOutput(const QString &value);

    struct StreamSpec {
        int width = 0;
        int height = 0;
        int fps = 30;
        QString output;
        bool paused = false;
        bool cpuFrames = false;
    };
    QByteArray workerKey() const;
    StreamSpec streamSpec() const;
    QByteArray sharedImageDevice() const;
    QByteArray sharedImageDriver() const;
    bool sharesWorker() const;
    bool workerReady() const;
    int beginSharedStream();
    int beginSharedFrames();
    void sharedWorkerFailed(const QString &error);
    void sharedWorkerFinished(int code, QProcess::ExitStatus status, const QByteArray &errors);

signals:
    void presentationIdChanged();
    void assignmentChanged();
    void paperChanged();
    void streamSizeChanged();
    void streamFpsChanged();
    void pausedChanged();
    void outputChanged();

protected:
    void componentComplete() override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void setSharedImageDevice(const QByteArray &uuid, const QByteArray &driver);

public:
    struct DmabufSlot {
        int fd = -1;
        int semaphoreFd = -1;
        quint32 width = 0;
        quint32 height = 0;
        quint32 stride = 0;
        quint32 offset = 0;
        quint64 modifier = 0;
        bool opaque = false;
    };

    void scheduleRestart();

private:
    void createProcess();
    void restart();
    bool retune(const QString &previous);
    void resetStream();
    int openStream();
    bool poolEligible() const;
    void sendControl(const QByteArray &line);
    void sendPause();
    void sendPointer(const QPointF &scenePosition, Qt::MouseButtons buttons, bool buttonsChanged);
    void consume();
    void consumeFrames(const QByteArray &bytes);
    void readFrames();
    void consumeDmabuf();
    void closeDmabuf();
    void acknowledge(int slot);
    void collectErrors();
    void failPresentation(const QString &error);
    void reportPresentation();
    void frameAccepted();
    void scheduleFramePoll();
    bool slotsExhausted() const;
    void frameFailed(const QString &error);

    QString m_presentationId;
    QByteArray m_stderr;
    QString m_error;
    bool m_workerReady = false;
    bool m_frameAccepted = false;
    quint64 m_frameQueuedGeneration = 0;
    quint64 m_errorQueuedGeneration = 0;
    QString m_assignment;
    QString m_paper = QStringLiteral("/usr/bin/skwd-paper-v2");
    int m_streamWidth = 1280;
    int m_streamHeight = 720;
    int m_streamFps = 30;
    bool m_paused = false;
    QString m_output;
    bool m_pooled = false;
    bool m_restartScheduled = false;
    bool m_restartRequired = false;
    bool m_stopping = false;
    bool m_destroying = false;
    QProcess *m_process = nullptr;
    QByteArray m_buffer;
    QByteArray m_frame;
    int m_headerWidth = 0;
    int m_headerHeight = 0;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    QMutex m_frameMutex;
    std::array<DmabufSlot, 3> m_slots;
    std::array<bool, 3> m_outstandingSlots {};
    std::array<bool, 3> m_ackSlots {};
    QSocketNotifier *m_socketNotifier = nullptr;
    int m_socket = -1;
    QSocketNotifier *m_frameNotifier = nullptr;
    int m_frameSocket = -1;
    std::vector<int> m_pendingSlots;
    qsizetype m_frameBytes = 0;
    quint64 m_generation = 0;
    quint64 m_streamGeneration = 0;
    quint16 m_epoch = 0;
    bool m_deviceKnown = false;
    bool m_stillStream = false;
    bool m_gpuUnavailable = false;
    QByteArray m_deviceUuid;
    QByteArray m_driverUuid;
    bool m_ready = false;
    bool m_framePollScheduled = false;
    bool m_lateAcks = false;
    QQuickWindow *m_pointerWindow = nullptr;
    QElapsedTimer m_pointerTimer;
    quint32 m_pointerLast = 0;
    quint8 m_pointerButtons = 0;
};
