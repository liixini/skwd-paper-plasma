#include "skwdworkerpool.h"
#include "skwdvideoitem.h"
#include "skwdprocess.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QProcessEnvironment>
#include <QTimer>
#include <array>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr int MaxSharedStreams = 32;
}

SkwdWorkerPool *SkwdWorkerPool::instance()
{
    static SkwdWorkerPool *pool = new SkwdWorkerPool(QCoreApplication::instance());
    return pool;
}

SkwdWorkerPool::SkwdWorkerPool(QObject *parent)
    : QObject(parent)
{
}

int SkwdWorkerPool::workerCount() const
{
    return m_workers.size();
}

SkwdWorkerPool::Worker *SkwdWorkerPool::workerFor(const QByteArray &key)
{
    auto *worker = m_workers.value(key);
    if (!worker) {
        worker = new Worker;
        worker->key = key;
        createProcess(worker);
        m_workers.insert(key, worker);
    }
    return worker;
}

void SkwdWorkerPool::createProcess(Worker *worker)
{
    worker->process = new QProcess(this);
    worker->process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(worker->process, &QProcess::readyReadStandardOutput, this, [worker] {
        worker->process->readAllStandardOutput();
    });
    connect(worker->process, &QProcess::readyReadStandardError, this, [worker] {
        worker->errors.append(worker->process->readAllStandardError());
        worker->errors = worker->errors.right(8192);
    });
    connect(worker->process, &QProcess::errorOccurred, this, [this, worker](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || worker->stopping) {
            return;
        }
        const auto members = worker->members;
        for (auto *member : members) {
            member->sharedWorkerFailed(QStringLiteral("Cannot start %1: %2")
                .arg(worker->process->program(), worker->process->errorString()));
        }
    });
    connect(worker->process, &QProcess::finished, this, [this, worker](int code, QProcess::ExitStatus status) {
        finished(worker, code, status);
    });
}

void SkwdWorkerPool::update(SkwdVideoItem *item)
{
    const QByteArray key = item->workerKey();
    auto *current = m_membership.value(item);
    if (current && current->key != key) {
        current->members.removeAll(item);
        m_membership.remove(item);
        if (current->members.isEmpty()) {
            release(current);
        } else {
            scheduleSpawn(current);
        }
        current = nullptr;
    }
    if (!current) {
        current = workerFor(key);
        current->members.append(item);
        m_membership.insert(item, current);
    }
    scheduleSpawn(current);
}

bool SkwdWorkerPool::retune(SkwdVideoItem *item, const QByteArray &line)
{
    auto *worker = m_membership.value(item);
    if (!worker || worker->process->state() != QProcess::Running) {
        return false;
    }
    const QByteArray key = item->workerKey();
    if (worker->spawnScheduled
        || (worker->key != key && (worker->members.size() > 1 || m_workers.contains(key)))) {
        update(item);
        return true;
    }
    if (worker->key != key) {
        m_workers.remove(worker->key);
        worker->key = key;
        m_workers.insert(key, worker);
    }
    return line.isEmpty() || sendControl(item, line);
}

void SkwdWorkerPool::detach(SkwdVideoItem *item)
{
    auto *worker = m_membership.take(item);
    if (!worker) {
        return;
    }
    worker->members.removeAll(item);
    if (worker->members.isEmpty()) {
        release(worker);
    } else {
        scheduleSpawn(worker);
    }
}

bool SkwdWorkerPool::sendControl(SkwdVideoItem *item, const QByteArray &line)
{
    auto *worker = m_membership.value(item);
    if (!worker || worker->process->state() != QProcess::Running) {
        return false;
    }
    return worker->process->write(line) >= 0;
}

void SkwdWorkerPool::scheduleSpawn(Worker *worker)
{
    if (worker->spawnScheduled) {
        return;
    }
    worker->spawnScheduled = true;
    const QByteArray key = worker->key;
    QTimer::singleShot(0, this, [this, key, worker] {
        if (m_workers.value(key) != worker) {
            return;
        }
        worker->spawnScheduled = false;
        spawn(worker);
    });
}

void SkwdWorkerPool::stop(Worker *worker)
{
    retireSkwdProcess(worker->process);
    worker->process = nullptr;
    worker->errors.clear();
}

void SkwdWorkerPool::release(Worker *worker)
{
    m_workers.remove(worker->key);
    stop(worker);
    delete worker;
}

bool SkwdWorkerPool::legacyPresenter(const QString &paper) const
{
    return m_legacyPapers.contains(paper);
}

void SkwdWorkerPool::finished(Worker *worker, int code, QProcess::ExitStatus status)
{
    if (worker->stopping) {
        return;
    }
    worker->errors.append(worker->process->readAllStandardError());
    const auto members = worker->members;
    if (code == 2 && status == QProcess::NormalExit && worker->errors.contains("unexpected argument '--stream'")) {
        qWarning() << "skwd-paper-plasma:" << worker->process->program()
                   << "predates shared presenters; using one presenter per output";
        m_legacyPapers.insert(worker->process->program());
        for (auto *member : members) {
            detach(member);
            member->scheduleRestart();
        }
        return;
    }
    for (auto *member : members) {
        member->sharedWorkerFinished(code, status, worker->errors);
    }
}

void SkwdWorkerPool::spawn(Worker *worker)
{
    stop(worker);
    createProcess(worker);
    if (worker->members.isEmpty()) {
        return;
    }
    QList<int> childSockets;
    QStringList arguments {QStringLiteral("present-plasma")};
    QJsonArray outputs;
    QStringList streams;
    for (auto *member : std::as_const(worker->members)) {
        if (childSockets.size() + 2 > MaxSharedStreams) {
            member->sharedWorkerFailed(QStringLiteral("Too many outputs share one wallpaper renderer"));
            continue;
        }
        const int child = member->beginSharedStream();
        if (child < 0) {
            continue;
        }
        const auto spec = member->streamSpec();
        outputs.append(spec.output);
        QString stream = QStringLiteral("fd=%1,size=%2x%3,fps=%4,output=%5,paused=%6")
            .arg(3 + childSockets.size()).arg(spec.width).arg(spec.height).arg(spec.fps)
            .arg(spec.output).arg(spec.paused ? 1 : 0);
        childSockets.append(child);
        if (spec.cpuFrames) {
            const int frames = member->beginSharedFrames();
            if (frames < 0) {
                continue;
            }
            stream += QStringLiteral(",frame_fd=%1").arg(3 + childSockets.size());
            childSockets.append(frames);
        }
        streams.append(stream);
    }
    if (childSockets.isEmpty()) {
        return;
    }
    auto *lead = worker->members.first();
    QJsonObject assignment = QJsonDocument::fromJson(lead->assignment().toUtf8()).object();
    assignment.insert(QStringLiteral("outputs"), outputs);
    arguments << QStringLiteral("--assignment")
              << QString::fromUtf8(QJsonDocument(assignment).toJson(QJsonDocument::Compact));
    for (const auto &stream : streams) {
        arguments << QStringLiteral("--stream") << stream;
    }
    std::array<int, MaxSharedStreams> sockets {};
    sockets.fill(-1);
    for (int index = 0; index < childSockets.size(); ++index) {
        sockets[index] = childSockets[index];
    }
    const int count = childSockets.size();
    worker->process->setChildProcessModifier([sockets, count] {
        std::array<int, MaxSharedStreams> parked {};
        for (int index = 0; index < count; ++index) {
            parked[index] = ::fcntl(sockets[index], F_DUPFD_CLOEXEC, 3 + count);
            if (parked[index] < 0) ::_exit(127);
        }
        for (int index = 0; index < count; ++index) {
            if (::dup2(parked[index], 3 + index) < 0) ::_exit(127);
            ::close(parked[index]);
        }
        if (!isolateSkwdProcessDescriptors(3 + count)) ::_exit(127);
    });
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SKWD_PAPER_PLASMA_GPU_STREAM"),
        lead->sharedImageDevice().isEmpty() ? QStringLiteral("0") : QStringLiteral("1"));
    environment.insert(QStringLiteral("SKWD_PAPER_PLASMA_DEVICE_UUID"), QString::fromLatin1(lead->sharedImageDevice()));
    environment.insert(QStringLiteral("SKWD_PAPER_PLASMA_DRIVER_UUID"), QString::fromLatin1(lead->sharedImageDriver()));
    worker->process->setProcessEnvironment(environment);
    worker->process->start(lead->paper(), arguments);
    for (int socket : childSockets) {
        ::close(socket);
    }
}
