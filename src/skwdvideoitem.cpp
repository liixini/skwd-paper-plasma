#include "skwdvideoitem.h"
#include "skwdworkerpool.h"
#include "skwdprocess.h"

#include <QImage>
#include <QElapsedTimer>
#include <QOpenGLExtraFunctions>
#include <QProcessEnvironment>
#include <QSGRendererInterface>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QtGui/qopenglcontext_platform.h>
#include <QtQuick/qsgtexture_platform.h>
#include <QEvent>
#include <QMouseEvent>
#include <QSocketNotifier>
#include <QTimer>
#include <QtEndian>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <drm_fourcc.h>
#include <array>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

namespace {
constexpr int SlotCount = 3;
constexpr int MinStreamEdge = 16;
constexpr int MaxStreamEdge = 8192;
constexpr qint64 FrameStreamPixels = qint64(3840) * 2160;

struct ReceivedDescriptor {
    int fd = -1;
    ~ReceivedDescriptor() { if (fd >= 0) ::close(fd); }
};

quint32 read32(const uchar *data)
{
    return qFromLittleEndian<quint32>(data);
}

quint64 read64(const uchar *data)
{
    return qFromLittleEndian<quint64>(data);
}

QSize fitFrameStream(int width, int height)
{
    if (qint64(width) * height <= FrameStreamPixels) {
        return {width, height};
    }
    const double scale = std::sqrt(double(FrameStreamPixels) / (double(width) * double(height)));
    width = qMax(MinStreamEdge, int(width * scale));
    height = qMax(MinStreamEdge, int(height * scale));
    while (qint64(width) * height > FrameStreamPixels) {
        if (width >= height) {
            --width;
        } else {
            --height;
        }
    }
    return {width, height};
}

class SkwdFrameNode final : public QSGSimpleTextureNode {
public:
    struct Texture {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint name = 0;
        GLuint memory = 0;
        GLuint semaphore = 0;
        QSGTexture *texture = nullptr;
        GLsync retired = nullptr;
        QElapsedTimer retirementAge;
    };

    ~SkwdFrameNode() override
    {
        const bool hasContext = QOpenGLContext::currentContext() != nullptr;
        for (auto &slot : textures) {
            if (hasContext && slot.retired) {
                QOpenGLContext::currentContext()->extraFunctions()->glDeleteSync(slot.retired);
            }
            delete slot.texture;
            if (hasContext && slot.name != 0) {
                glDeleteTextures(1, &slot.name);
            }
            if (hasContext && slot.memory != 0 && deleteMemoryObjects) {
                deleteMemoryObjects(1, &slot.memory);
            }
            if (hasContext && slot.semaphore != 0 && deleteSemaphores) {
                deleteSemaphores(1, &slot.semaphore);
            }
            if (hasContext && slot.image != EGL_NO_IMAGE_KHR && destroyImage) {
                destroyImage(display, slot.image);
            }
        }
    }

    bool import(int index, SkwdVideoItem::DmabufSlot &slot, QQuickWindow *window)
    {
        if (textures[index].texture) {
            return true;
        }
        auto *context = QOpenGLContext::currentContext();
        if (!context) {
            return false;
        }
        auto *native = context->nativeInterface<QNativeInterface::QEGLContext>();
        if (!native) {
            return false;
        }
        display = native->display();
        createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        imageTarget = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!createImage || !destroyImage || !imageTarget) {
            return false;
        }
        const EGLint attributes[] = {
            EGL_WIDTH, EGLint(slot.width),
            EGL_HEIGHT, EGLint(slot.height),
            EGL_LINUX_DRM_FOURCC_EXT, EGLint(DRM_FORMAT_XRGB8888),
            EGL_DMA_BUF_PLANE0_FD_EXT, slot.fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(slot.offset),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(slot.stride),
            EGL_NONE,
        };
        auto &target = textures[index];
        if (slot.opaque) {
            createMemoryObjects = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(
                eglGetProcAddress("glCreateMemoryObjectsEXT"));
            deleteMemoryObjects = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(
                eglGetProcAddress("glDeleteMemoryObjectsEXT"));
            memoryParameter = reinterpret_cast<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(
                eglGetProcAddress("glMemoryObjectParameterivEXT"));
            importMemoryFd = reinterpret_cast<PFNGLIMPORTMEMORYFDEXTPROC>(
                eglGetProcAddress("glImportMemoryFdEXT"));
            textureStorageMemory = reinterpret_cast<PFNGLTEXTURESTORAGEMEM2DEXTPROC>(
                eglGetProcAddress("glTextureStorageMem2DEXT"));
            genSemaphores = reinterpret_cast<PFNGLGENSEMAPHORESEXTPROC>(
                eglGetProcAddress("glGenSemaphoresEXT"));
            deleteSemaphores = reinterpret_cast<PFNGLDELETESEMAPHORESEXTPROC>(
                eglGetProcAddress("glDeleteSemaphoresEXT"));
            importSemaphoreFd = reinterpret_cast<PFNGLIMPORTSEMAPHOREFDEXTPROC>(
                eglGetProcAddress("glImportSemaphoreFdEXT"));
            waitSemaphore = reinterpret_cast<PFNGLWAITSEMAPHOREEXTPROC>(
                eglGetProcAddress("glWaitSemaphoreEXT"));
            if (!createMemoryObjects || !deleteMemoryObjects || !memoryParameter
                || !importMemoryFd || !textureStorageMemory || !genSemaphores
                || !deleteSemaphores || !importSemaphoreFd || !waitSemaphore
                || slot.semaphoreFd < 0) {
                return false;
            }
            createMemoryObjects(1, &target.memory);
            const GLint dedicated = GL_TRUE;
            memoryParameter(target.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
            importMemoryFd(target.memory, slot.modifier, GL_HANDLE_TYPE_OPAQUE_FD_EXT, slot.fd);
            slot.fd = -1;
            glGenTextures(1, &target.name);
            glBindTexture(GL_TEXTURE_2D, target.name);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
            glBindTexture(GL_TEXTURE_2D, 0);
            textureStorageMemory(target.name, 1, GL_RGBA8, slot.width, slot.height, target.memory, 0);
            const GLenum error = glGetError();
            qInfo() << "skwd-wallpaper: GL memory-object import" << error;
            if (error != GL_NO_ERROR) {
                return false;
            }
            target.texture = QNativeInterface::QSGOpenGLTexture::fromNative(
                target.name, window, QSize(int(slot.width), int(slot.height)));
            if (!target.texture) {
                return false;
            }
            genSemaphores(1, &target.semaphore);
            importSemaphoreFd(target.semaphore, GL_HANDLE_TYPE_OPAQUE_FD_EXT, slot.semaphoreFd);
            slot.semaphoreFd = -1;
            target.texture->setFiltering(QSGTexture::Linear);
            qInfo() << "skwd-wallpaper: imported Vulkan memory slot" << index;
            return true;
        }
        target.image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
        if (target.image == EGL_NO_IMAGE_KHR) {
            qWarning() << "skwd-wallpaper: EGL DMA-BUF import failed" << eglGetError();
            return false;
        }
        glGenTextures(1, &target.name);
        glBindTexture(GL_TEXTURE_2D, target.name);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        imageTarget(GL_TEXTURE_2D, target.image);
        qInfo() << "skwd-wallpaper: GL" << reinterpret_cast<const char *>(glGetString(GL_VENDOR))
                << reinterpret_cast<const char *>(glGetString(GL_RENDERER)) << glGetError();
        glBindTexture(GL_TEXTURE_2D, 0);
        target.texture = QNativeInterface::QSGOpenGLTexture::fromNative(
            target.name, window, QSize(int(slot.width), int(slot.height)));
        if (!target.texture) {
            return false;
        }
        target.texture->setFiltering(QSGTexture::Linear);
        ::close(slot.fd);
        slot.fd = -1;
        qInfo() << "skwd-wallpaper: imported DMA-BUF slot" << index;
        return true;
    }

    bool wait(int index)
    {
        auto &target = textures[index];
        if (!waitSemaphore || target.semaphore == 0 || target.name == 0) {
            return false;
        }
        const GLenum layout = GL_LAYOUT_GENERAL_EXT;
        waitSemaphore(target.semaphore, 0, nullptr, 1, &target.name, &layout);
        return glGetError() == GL_NO_ERROR;
    }

    bool retire(int index)
    {
        auto &slot = textures[index];
        slot.retired = QOpenGLContext::currentContext()->extraFunctions()->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        slot.retirementAge.start();
        return slot.retired != nullptr;
    }

    GLenum retirementStatus(int index)
    {
        auto &slot = textures[index];
        auto *gl = QOpenGLContext::currentContext()->extraFunctions();
        const GLenum status = gl->glClientWaitSync(slot.retired, 0, 0);
        if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
            gl->glDeleteSync(slot.retired);
            slot.retired = nullptr;
        }
        return status;
    }

    std::array<Texture, SlotCount> textures;
    EGLDisplay display = EGL_NO_DISPLAY;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget = nullptr;
    PFNGLCREATEMEMORYOBJECTSEXTPROC createMemoryObjects = nullptr;
    PFNGLDELETEMEMORYOBJECTSEXTPROC deleteMemoryObjects = nullptr;
    PFNGLMEMORYOBJECTPARAMETERIVEXTPROC memoryParameter = nullptr;
    PFNGLIMPORTMEMORYFDEXTPROC importMemoryFd = nullptr;
    PFNGLTEXTURESTORAGEMEM2DEXTPROC textureStorageMemory = nullptr;
    PFNGLGENSEMAPHORESEXTPROC genSemaphores = nullptr;
    PFNGLDELETESEMAPHORESEXTPROC deleteSemaphores = nullptr;
    PFNGLIMPORTSEMAPHOREFDEXTPROC importSemaphoreFd = nullptr;
    PFNGLWAITSEMAPHOREEXTPROC waitSemaphore = nullptr;
    quint64 generation = 0;
    quint64 streamGeneration = 0;
    int currentSlot = -1;
};
}

SkwdVideoItem::SkwdVideoItem(QQuickItem *parent)
    : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    createProcess();
}

void SkwdVideoItem::createProcess()
{
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &SkwdVideoItem::consume);
    connect(m_process, &QProcess::readyReadStandardError, this, &SkwdVideoItem::collectErrors);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            failPresentation(QStringLiteral("Cannot start %1: %2").arg(m_paper, m_process->errorString()));
        }
    });
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (m_stopping || m_destroying || m_assignment.isEmpty()) {
            return;
        }
        collectErrors();
        if (code != 0 || status != QProcess::NormalExit || !m_workerReady) {
            failPresentation(QStringLiteral("Wallpaper renderer exited (%1): %2")
                .arg(code).arg(QString::fromUtf8(m_stderr).trimmed()));
        }
    });
}

SkwdVideoItem::~SkwdVideoItem()
{
    m_destroying = true;
    m_stopping = true;
    if (m_pooled) {
        m_pooled = false;
        SkwdWorkerPool::instance()->detach(this);
    }
    closeDmabuf();
    retireSkwdProcess(m_process);
}

QString SkwdVideoItem::presentationId() const
{
    return m_presentationId;
}

void SkwdVideoItem::setPresentationId(const QString &value)
{
    if (m_presentationId == value) {
        return;
    }
    m_presentationId = value;
    emit presentationIdChanged();
    if (!m_error.isEmpty() && isComponentComplete()) {
        scheduleRestart();
    }
    QTimer::singleShot(0, this, [this] {
        if (!m_restartRequired) {
            reportPresentation();
        }
    });
}

void SkwdVideoItem::collectErrors()
{
    m_stderr.append(m_process->readAllStandardError());
    m_stderr = m_stderr.right(8192);
}

void SkwdVideoItem::failPresentation(const QString &error)
{
    if (m_stopping || m_destroying || !m_error.isEmpty()) {
        return;
    }
    qWarning().noquote() << "skwd-paper-plasma:" << error;
    const QString marker = QStringLiteral("exited with error: ");
    const auto detail = error.lastIndexOf(marker);
    m_error = (detail >= 0 ? error.mid(detail + marker.size()) : error).left(2048).trimmed();
    reportPresentation();
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
    }
}

void SkwdVideoItem::reportPresentation()
{
    if (m_restartRequired || m_presentationId.isEmpty() || m_presentationId.size() > 100
        || std::any_of(m_presentationId.begin(), m_presentationId.end(), [](QChar ch) {
            return (ch < QLatin1Char('0') || ch > QLatin1Char('9')) && ch != QLatin1Char('-');
        })) {
        return;
    }
    if (m_error.isEmpty() && (!m_workerReady || !m_frameAccepted)) {
        return;
    }
    const QString path = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
        + QStringLiteral("/skwd-paper-plasma/") + m_presentationId + QStringLiteral(".json");
    if (!QFile::exists(path)) {
        return;
    }
    QJsonObject status;
    status.insert(QStringLiteral("state"), m_error.isEmpty() ? QStringLiteral("ready") : QStringLiteral("error"));
    status.insert(QStringLiteral("error"), m_error);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(QJsonDocument(status).toJson(QJsonDocument::Compact)) < 0
        || !file.commit()) {
        qWarning() << "skwd-paper-plasma: cannot report presentation:" << file.errorString();
    }
}

void SkwdVideoItem::frameAccepted()
{
    const auto generation = m_streamGeneration;
    if (m_frameQueuedGeneration == generation) {
        return;
    }
    m_frameQueuedGeneration = generation;
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation == m_streamGeneration) {
            m_frameAccepted = true;
            reportPresentation();
        }
    }, Qt::QueuedConnection);
}

void SkwdVideoItem::frameFailed(const QString &error)
{
    const auto generation = m_streamGeneration;
    if (m_errorQueuedGeneration == generation) {
        return;
    }
    m_errorQueuedGeneration = generation;
    QMetaObject::invokeMethod(this, [this, generation, error] {
        if (generation == m_streamGeneration) {
            failPresentation(error);
        }
    }, Qt::QueuedConnection);
}

QString SkwdVideoItem::assignment() const
{
    return m_assignment;
}

void SkwdVideoItem::setAssignment(const QString &value)
{
    if (m_assignment == value) {
        return;
    }
    const QString previous = std::exchange(m_assignment, value);
    emit assignmentChanged();
    if (isComponentComplete() && !retune(previous)) {
        scheduleRestart();
    }
}

bool SkwdVideoItem::retune(const QString &previous)
{
    if (!m_workerReady || !m_error.isEmpty() || m_restartRequired) {
        return false;
    }
    const QJsonObject before = QJsonDocument::fromJson(previous.toUtf8()).object();
    const QJsonObject after = QJsonDocument::fromJson(m_assignment.toUtf8()).object();
    auto identity = [](QJsonObject assignment) {
        for (const auto key : {QStringLiteral("transition"), QStringLiteral("mute"), QStringLiteral("volume")}) {
            assignment.remove(key);
        }
        return assignment;
    };
    if (before.isEmpty() || identity(before) != identity(after)) {
        return false;
    }
    const auto kind = after.value(QStringLiteral("source")).toObject().value(QStringLiteral("kind")).toString();
    const bool audible = kind == QStringLiteral("video") || kind == QStringLiteral("we");
    QByteArray line;
    if (audible && (before.value(QStringLiteral("mute")) != after.value(QStringLiteral("mute"))
                    || before.value(QStringLiteral("volume")) != after.value(QStringLiteral("volume")))) {
        QJsonObject command;
        command.insert(QStringLiteral("to"), QString());
        command.insert(QStringLiteral("mute"), after.value(QStringLiteral("mute")).toBool(true));
        command.insert(QStringLiteral("volume"), after.value(QStringLiteral("volume")).toInt(80));
        line = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    }
    if (m_pooled) {
        return SkwdWorkerPool::instance()->retune(this, line);
    }
    if (!line.isEmpty()) {
        sendControl(line);
    }
    return true;
}

QString SkwdVideoItem::paper() const
{
    return m_paper;
}

void SkwdVideoItem::setPaper(const QString &value)
{
    if (m_paper == value) {
        return;
    }
    m_paper = value;
    emit paperChanged();
    if (isComponentComplete()) {
        scheduleRestart();
    }
}

int SkwdVideoItem::streamWidth() const
{
    return m_streamWidth;
}

void SkwdVideoItem::setStreamWidth(int value)
{
    value = qBound(MinStreamEdge, value, MaxStreamEdge);
    if (m_streamWidth == value) {
        return;
    }
    m_streamWidth = value;
    emit streamSizeChanged();
    if (isComponentComplete()) {
        scheduleRestart();
    }
}

int SkwdVideoItem::streamHeight() const
{
    return m_streamHeight;
}

void SkwdVideoItem::setStreamHeight(int value)
{
    value = qBound(MinStreamEdge, value, MaxStreamEdge);
    if (m_streamHeight == value) {
        return;
    }
    m_streamHeight = value;
    emit streamSizeChanged();
    if (isComponentComplete()) {
        scheduleRestart();
    }
}

int SkwdVideoItem::streamFps() const
{
    return m_streamFps;
}

void SkwdVideoItem::setStreamFps(int value)
{
    value = qBound(1, value, 240);
    if (m_streamFps == value) {
        return;
    }
    m_streamFps = value;
    emit streamFpsChanged();
    if (isComponentComplete()) {
        scheduleRestart();
    }
}

bool SkwdVideoItem::paused() const
{
    return m_paused;
}

void SkwdVideoItem::setPaused(bool value)
{
    if (m_paused == value) {
        return;
    }
    m_paused = value;
    emit pausedChanged();
    if (isComponentComplete()) {
        sendPause();
    }
}

void SkwdVideoItem::componentComplete()
{
    QQuickItem::componentComplete();
    restart();
}

void SkwdVideoItem::itemChange(ItemChange change, const ItemChangeData &value)
{
    QQuickItem::itemChange(change, value);
    if (change != ItemSceneChange) {
        return;
    }
    if (m_pointerWindow) {
        m_pointerWindow->removeEventFilter(this);
    }
    m_pointerWindow = value.window;
    if (m_pointerWindow) {
        m_pointerWindow->installEventFilter(this);
        m_pointerTimer.start();
    }
}

bool SkwdVideoItem::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_pointerWindow) {
        return false;
    }
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        sendPointer(mouse->scenePosition(), mouse->buttons(), event->type() != QEvent::MouseMove);
        break;
    }
    default:
        break;
    }
    return false;
}

static quint8 pointerButtonMask(Qt::MouseButtons buttons)
{
    quint8 mask = 0;
    if (buttons & Qt::LeftButton) mask |= 1;
    if (buttons & Qt::RightButton) mask |= 2;
    if (buttons & Qt::MiddleButton) mask |= 4;
    return mask;
}

void SkwdVideoItem::sendPointer(const QPointF &scenePosition, Qt::MouseButtons buttons, bool buttonsChanged)
{
    if (width() <= 0 || height() <= 0 || !m_process || m_process->state() != QProcess::Running) {
        return;
    }
    const QPointF local = mapFromScene(scenePosition);
    const quint16 x = quint16(qBound(0.0, local.x() / width(), 1.0) * 65535.0 + 0.5);
    const quint16 y = quint16(qBound(0.0, local.y() / height(), 1.0) * 65535.0 + 0.5);
    const quint32 packed = (quint32(x) << 16) | y;
    const quint8 mask = pointerButtonMask(buttons);
    if (!buttonsChanged && packed == m_pointerLast && mask == m_pointerButtons) {
        return;
    }
    if (!buttonsChanged && m_pointerTimer.isValid() && m_pointerTimer.elapsed() < 16) {
        return;
    }
    m_pointerTimer.restart();
    m_pointerLast = packed;
    m_pointerButtons = mask;
    QJsonObject pointer;
    pointer.insert(QStringLiteral("x"), int(x));
    pointer.insert(QStringLiteral("y"), int(y));
    pointer.insert(QStringLiteral("buttons"), int(mask));
    QJsonObject command;
    command.insert(QStringLiteral("to"), m_pooled ? m_output : QString());
    command.insert(QStringLiteral("pointer"), pointer);
    const QByteArray line = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    if (m_pooled) {
        SkwdWorkerPool::instance()->sendControl(this, line);
        return;
    }
    sendControl(line);
}

void SkwdVideoItem::scheduleRestart()
{
    m_restartRequired = true;
    if (m_restartScheduled) {
        return;
    }
    m_restartScheduled = true;
    QTimer::singleShot(0, this, [this] {
        m_restartScheduled = false;
        if (m_restartRequired) {
            m_restartRequired = false;
            restart();
        }
    });
}

void SkwdVideoItem::sendControl(const QByteArray &line)
{
    if (m_process->state() == QProcess::Running && m_process->write(line) < 0) {
        scheduleRestart();
    }
}

void SkwdVideoItem::sendPause()
{
    QJsonObject command;
    command.insert(QStringLiteral("to"), m_pooled ? m_output : QString());
    command.insert(QStringLiteral("pause"), m_paused);
    const QByteArray line = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    if (m_pooled) {
        if (!SkwdWorkerPool::instance()->sendControl(this, line)) {
            scheduleRestart();
        }
        return;
    }
    sendControl(line);
}

QString SkwdVideoItem::output() const
{
    return m_output;
}

void SkwdVideoItem::setOutput(const QString &value)
{
    if (m_output == value) {
        return;
    }
    m_output = value;
    emit outputChanged();
    if (isComponentComplete()) {
        scheduleRestart();
    }
}

bool SkwdVideoItem::poolEligible() const
{
    if (m_output.isEmpty() || m_assignment.isEmpty() || m_paper.isEmpty()
        || SkwdWorkerPool::instance()->legacyPresenter(m_paper)) {
        return false;
    }
    const auto source = QJsonDocument::fromJson(m_assignment.toUtf8()).object().value(QStringLiteral("source")).toObject();
    const QString kind = source.value(QStringLiteral("kind")).toString();
    if (kind == QStringLiteral("static")) {
        return true;
    }
    if (m_deviceUuid.isEmpty()) {
        return false;
    }
    if (kind == QStringLiteral("video")) {
        return source.value(QStringLiteral("engine")).toString() != QStringLiteral("tinier");
    }
    return kind == QStringLiteral("we");
}

bool SkwdVideoItem::sharesWorker() const
{
    return m_pooled;
}

bool SkwdVideoItem::workerReady() const
{
    return m_workerReady;
}

QByteArray SkwdVideoItem::workerKey() const
{
    QJsonObject assignment = QJsonDocument::fromJson(m_assignment.toUtf8()).object();
    assignment.remove(QStringLiteral("outputs"));
    return m_paper.toUtf8() + '\n' + m_deviceUuid + '\n'
        + QJsonDocument(assignment).toJson(QJsonDocument::Compact);
}

SkwdVideoItem::StreamSpec SkwdVideoItem::streamSpec() const
{
    const auto source = QJsonDocument::fromJson(m_assignment.toUtf8()).object().value(QStringLiteral("source")).toObject();
    const QString kind = source.value(QStringLiteral("kind")).toString();
    const bool cpuFrames = kind == QStringLiteral("static");
    const bool budgeted = cpuFrames
        || (kind == QStringLiteral("video") && source.value(QStringLiteral("engine")).toString() == QStringLiteral("tinier"));
    const QSize size = budgeted ? fitFrameStream(m_streamWidth, m_streamHeight) : QSize(m_streamWidth, m_streamHeight);
    return {size.width(), size.height(), m_streamFps, m_output, m_paused, cpuFrames};
}

int SkwdVideoItem::beginSharedFrames()
{
    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC) != 0) {
        failPresentation(QStringLiteral("Cannot create the wallpaper frame pipe"));
        return -1;
    }
    m_frameSocket = pipes[0];
    const int flags = ::fcntl(m_frameSocket, F_GETFL, 0);
    ::fcntl(m_frameSocket, F_SETFL, flags | O_NONBLOCK);
    m_frameNotifier = new QSocketNotifier(m_frameSocket, QSocketNotifier::Read, this);
    connect(m_frameNotifier, &QSocketNotifier::activated, this, &SkwdVideoItem::readFrames);
    return pipes[1];
}

void SkwdVideoItem::readFrames()
{
    QByteArray bytes;
    std::array<char, 65536> chunk {};
    while (bytes.size() < 1024 * 1024) {
        const ssize_t length = ::read(m_frameSocket, chunk.data(), chunk.size());
        if (length > 0) {
            bytes.append(chunk.data(), length);
            continue;
        }
        if (length < 0 && errno == EINTR) {
            continue;
        }
        if (length == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            m_frameNotifier->setEnabled(false);
        }
        break;
    }
    if (!bytes.isEmpty()) {
        consumeFrames(bytes);
    }
}

QByteArray SkwdVideoItem::sharedImageDevice() const
{
    return m_deviceUuid;
}

QByteArray SkwdVideoItem::sharedImageDriver() const
{
    return m_driverUuid;
}

void SkwdVideoItem::setSharedImageDevice(const QByteArray &uuid, const QByteArray &driver)
{
    m_deviceUuid = uuid;
    m_driverUuid = driver;
    m_deviceKnown = true;
}

int SkwdVideoItem::beginSharedStream()
{
    resetStream();
    return openStream();
}

void SkwdVideoItem::sharedWorkerFailed(const QString &error)
{
    failPresentation(error);
}

void SkwdVideoItem::sharedWorkerFinished(int code, QProcess::ExitStatus status, const QByteArray &errors)
{
    if (m_stopping || m_destroying || m_assignment.isEmpty()) {
        return;
    }
    if (code != 0 || status != QProcess::NormalExit || !m_workerReady) {
        failPresentation(QStringLiteral("Wallpaper renderer exited (%1): %2")
            .arg(code).arg(QString::fromUtf8(errors).trimmed()));
    }
}

void SkwdVideoItem::closeDmabuf()
{
    delete m_socketNotifier;
    m_socketNotifier = nullptr;
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
    }
    delete m_frameNotifier;
    m_frameNotifier = nullptr;
    if (m_frameSocket >= 0) {
        ::close(m_frameSocket);
        m_frameSocket = -1;
    }
    QMutexLocker lock(&m_frameMutex);
    for (auto &slot : m_slots) {
        if (slot.fd >= 0) {
            ::close(slot.fd);
        }
        if (slot.semaphoreFd >= 0) {
            ::close(slot.semaphoreFd);
        }
        slot = {};
    }
    m_pendingSlots.clear();
    m_outstandingSlots.fill(false);
    m_ackSlots.fill(false);
}

void SkwdVideoItem::restart()
{
    if (!m_deviceKnown) {
        // Software scene-graph tests and CPU-only sessions need no GL device.
        if (window() && window()->rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
            m_deviceKnown = true;
        } else {
            update();
            return;
        }
    }
    m_stopping = true;
    retireSkwdProcess(m_process);
    createProcess();
    m_stopping = false;
    const bool pooled = poolEligible();
    if (m_pooled && !pooled) {
        m_pooled = false;
        SkwdWorkerPool::instance()->detach(this);
    }
    if (pooled) {
        m_pooled = true;
        SkwdWorkerPool::instance()->update(this);
        return;
    }
    resetStream();
    if (m_assignment.isEmpty() || m_paper.isEmpty()) {
        update();
        return;
    }
    const int childSocket = openStream();
    if (childSocket < 0) {
        return;
    }
    m_process->setChildProcessModifier([childSocket] {
        if (::dup2(childSocket, 3) < 0 || ::fcntl(3, F_SETFD, 0) < 0) ::_exit(127);
        if (childSocket != 3) {
            ::close(childSocket);
        }
        if (!isolateSkwdProcessDescriptors(4)) ::_exit(127);
    });
    const auto spec = streamSpec();
    QStringList arguments {QStringLiteral("present-plasma"),
            QStringLiteral("--assignment"), m_assignment,
            QStringLiteral("--stream-size"),
            QStringLiteral("%1x%2").arg(spec.width).arg(spec.height),
            QStringLiteral("--stream-fps"), QString::number(m_streamFps),
            QStringLiteral("--stream-fd"), QStringLiteral("3")};
    if (m_paused) {
        arguments.append(QStringLiteral("--paused"));
    }
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SKWD_PAPER_PLASMA_GPU_STREAM"), m_deviceUuid.isEmpty() ? QStringLiteral("0") : QStringLiteral("1"));
    if (!m_deviceUuid.isEmpty()) {
        environment.insert(QStringLiteral("SKWD_PAPER_PLASMA_DEVICE_UUID"), QString::fromLatin1(m_deviceUuid));
        environment.insert(QStringLiteral("SKWD_PAPER_PLASMA_DRIVER_UUID"), QString::fromLatin1(m_driverUuid));
    } else {
        environment.remove(QStringLiteral("SKWD_PAPER_PLASMA_DEVICE_UUID"));
        environment.remove(QStringLiteral("SKWD_PAPER_PLASMA_DRIVER_UUID"));
    }
    m_process->setProcessEnvironment(environment);
    m_process->start(m_paper, arguments);
    ::close(childSocket);
}

void SkwdVideoItem::resetStream()
{
    m_epoch = 0;
    ++m_streamGeneration;
    m_gpuUnavailable = false;
    m_lateAcks = false;
    m_stillStream = QJsonDocument::fromJson(m_assignment.toUtf8()).object().value(QStringLiteral("source")).toObject()
        .value(QStringLiteral("kind")).toString() == QStringLiteral("static");
    closeDmabuf();
    m_buffer.clear();
    m_stderr.clear();
    m_error.clear();
    m_workerReady = false;
    m_frameAccepted = false;
    m_headerWidth = 0;
    m_headerHeight = 0;
    {
        QMutexLocker lock(&m_frameMutex);
        m_frame.clear();
        m_frameWidth = 0;
        m_frameHeight = 0;
    }
    m_frameBytes = 0;
    m_ready = false;
}

int SkwdVideoItem::openStream()
{
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        failPresentation(QStringLiteral("Cannot create the wallpaper frame socket"));
        return -1;
    }
    m_socket = sockets[0];
    const int flags = ::fcntl(m_socket, F_GETFL, 0);
    ::fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
    m_socketNotifier = new QSocketNotifier(m_socket, QSocketNotifier::Read, this);
    connect(m_socketNotifier, &QSocketNotifier::activated, this, &SkwdVideoItem::consumeDmabuf);
    return sockets[1];
}

void SkwdVideoItem::consumeDmabuf()
{
    for (int batch = 0; batch < 32; ++batch) {
        std::array<uchar, 32> bytes {};
        std::array<uchar, CMSG_SPACE(sizeof(int))> control {};
        iovec iov {bytes.data(), bytes.size()};
        msghdr message {};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const ssize_t length = ::recvmsg(m_socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        ReceivedDescriptor received;
        int descriptorCount = 0;
        for (cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
            const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t index = 0; index < count; ++index) {
                int fd;
                std::memcpy(&fd, CMSG_DATA(header) + index * sizeof(int), sizeof(fd));
                if (++descriptorCount == 1) received.fd = fd;
                else ::close(fd);
            }
        }
        if (length <= 0) {
            m_socketNotifier->setEnabled(false);
            return;
        }
        if (message.msg_flags & (MSG_CTRUNC | MSG_TRUNC) || descriptorCount > 1) continue;
        if (length != 32 || std::memcmp(bytes.data(), "SKDG", 4) != 0) {
            continue;
        }
        const quint16 epoch = qFromLittleEndian<quint16>(bytes.data() + 6);
        if (bytes[4] == 7 && length == 32) {
            QMutexLocker lock(&m_frameMutex);
            for (auto &slot : m_slots) {
                if (slot.fd >= 0) ::close(slot.fd);
                if (slot.semaphoreFd >= 0) ::close(slot.semaphoreFd);
                slot = {};
            }
            m_pendingSlots.clear();
            m_outstandingSlots.fill(false);
            m_ackSlots.fill(false);
            m_frame.clear();
            m_buffer.clear();
            m_ready = false;
            m_epoch = epoch;
            ++m_streamGeneration;
            m_workerReady = false;
            m_frameAccepted = false;
            m_lateAcks = false;
            bytes[4] = 8;
            ::send(m_socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            continue;
        }
        if (epoch != m_epoch) continue;
        if (bytes[4] == 6 && length == 32) {
            m_lateAcks = (bytes[8] & 1) != 0;
            m_workerReady = true;
            reportPresentation();
            continue;
        }
        const int slotIndex = bytes[5];
        if (slotIndex < 0 || slotIndex >= SlotCount) {
            continue;
        }
        if ((bytes[4] == 1 || bytes[4] == 4 || bytes[4] == 5) && length == 32) {
            if (received.fd < 0) continue;
            QMutexLocker lock(&m_frameMutex);
            auto &slot = m_slots[slotIndex];
            if (bytes[4] == 5) {
                if (slot.semaphoreFd >= 0) ::close(slot.semaphoreFd);
                slot.semaphoreFd = std::exchange(received.fd, -1);
                continue;
            }
            if (slot.fd >= 0) ::close(slot.fd);
            slot.fd = std::exchange(received.fd, -1);
            slot.width = read32(bytes.data() + 8);
            slot.height = read32(bytes.data() + 12);
            slot.stride = read32(bytes.data() + 16);
            slot.offset = read32(bytes.data() + 20);
            slot.modifier = read64(bytes.data() + 24);
            slot.opaque = bytes[4] == 4;
        } else if (bytes[4] == 2) {
            if (m_outstandingSlots[slotIndex]) {
                failPresentation(QStringLiteral("Wallpaper renderer reused a GPU frame before release"));
                m_socketNotifier->setEnabled(false);
                return;
            }
            m_outstandingSlots[slotIndex] = true;
            {
                QMutexLocker lock(&m_frameMutex);
                m_pendingSlots.push_back(slotIndex);
                ++m_generation;
            }
            update();
        }
    }
}

void SkwdVideoItem::acknowledge(int slot)
{
    if (m_socket < 0 || slot < 0) {
        return;
    }
    std::array<uchar, 32> bytes {};
    std::memcpy(bytes.data(), "SKDG", 4);
    bytes[4] = 3;
    bytes[5] = uchar(slot);
    qToLittleEndian(m_epoch, bytes.data() + 6);
    const ssize_t sent = ::send(m_socket, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    m_ackSlots[slot] = sent != ssize_t(bytes.size());
    if (m_ackSlots[slot]) scheduleFramePoll();
    else m_outstandingSlots[slot] = false;
}

void SkwdVideoItem::consume()
{
    consumeFrames(m_process->read(1024 * 1024));
    if (m_process->bytesAvailable() > 0) QTimer::singleShot(0, this, &SkwdVideoItem::consume);
}

void SkwdVideoItem::consumeFrames(const QByteArray &bytes)
{
    m_buffer.append(bytes);
    if (!m_ready) {
        if (m_buffer.size() < 12) {
            return;
        }
        if (m_buffer.first(4) != QByteArrayLiteral("SKWP")) {
            failPresentation(QStringLiteral("Wallpaper renderer sent an invalid frame header"));
            return;
        }
        const auto *header = reinterpret_cast<const uchar *>(m_buffer.constData() + 4);
        const quint32 width = qFromLittleEndian<quint32>(header);
        const quint32 height = qFromLittleEndian<quint32>(header + 4);
        if (width == 0 || height == 0 || width > 7680 || height > 4320) {
            failPresentation(QStringLiteral("Wallpaper renderer sent an invalid frame size"));
            return;
        }
        m_frameBytes = qsizetype(width) * qsizetype(height) * 4;
        m_headerWidth = int(width);
        m_headerHeight = int(height);
        m_buffer.remove(0, 12);
        m_ready = true;
    }
    while (m_ready && m_buffer.size() >= m_frameBytes) {
        QByteArray frame = m_buffer.first(m_frameBytes);
        m_buffer.remove(0, m_frameBytes);
        {
            QMutexLocker lock(&m_frameMutex);
            m_frame = std::move(frame);
            m_frameWidth = m_headerWidth;
            m_frameHeight = m_headerHeight;
            ++m_generation;
        }
        update();
    }
}

bool SkwdVideoItem::slotsExhausted() const
{
    return std::all_of(m_outstandingSlots.begin(), m_outstandingSlots.end(), [](bool outstanding) { return outstanding; });
}

void SkwdVideoItem::scheduleFramePoll()
{
    const auto generation = m_streamGeneration;
    QMetaObject::invokeMethod(this, [this, generation] {
        if (generation != m_streamGeneration || m_framePollScheduled || !m_error.isEmpty()) return;
        m_framePollScheduled = true;
        QTimer::singleShot(16, this, [this, generation] {
            m_framePollScheduled = false;
            if (generation == m_streamGeneration && m_error.isEmpty()) update();
        });
    }, Qt::QueuedConnection);
}

static QSGNode *presentable(QSGSimpleTextureNode *node)
{
    if (node && !node->texture()) {
        delete node;
        return nullptr;
    }
    return node;
}

QSGNode *SkwdVideoItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    if (!m_deviceKnown) {
        QByteArray uuid, driver;
        if (auto *context = QOpenGLContext::currentContext()) {
            auto getIndexed = reinterpret_cast<PFNGLGETUNSIGNEDBYTEI_VEXTPROC>(context->getProcAddress("glGetUnsignedBytei_vEXT"));
            auto get = reinterpret_cast<PFNGLGETUNSIGNEDBYTEVEXTPROC>(context->getProcAddress("glGetUnsignedBytevEXT"));
            if (getIndexed && get
                && context->hasExtension(QByteArrayLiteral("GL_EXT_memory_object"))
                && context->hasExtension(QByteArrayLiteral("GL_EXT_memory_object_fd"))
                && context->hasExtension(QByteArrayLiteral("GL_EXT_semaphore"))
                && context->hasExtension(QByteArrayLiteral("GL_EXT_semaphore_fd"))) {
                GLint count = 0;
                glGetIntegerv(GL_NUM_DEVICE_UUIDS_EXT, &count);
                if (count == 1) {
                    std::array<GLubyte, 16> bytes {};
                    getIndexed(GL_DEVICE_UUID_EXT, 0, bytes.data());
                    uuid = QByteArray(reinterpret_cast<const char *>(bytes.data()), bytes.size()).toHex();
                    get(GL_DRIVER_UUID_EXT, bytes.data());
                    driver = QByteArray(reinterpret_cast<const char *>(bytes.data()), bytes.size()).toHex();
                    if (glGetError() != GL_NO_ERROR) { uuid.clear(); driver.clear(); }
                }
            }
        }
        m_deviceKnown = true;
        QMetaObject::invokeMethod(this, [this, uuid, driver] {
            m_deviceUuid = uuid;
            m_driverUuid = driver;
            qInfo() << "skwd-paper-plasma: shared-image device" << uuid;
            scheduleRestart();
        }, Qt::QueuedConnection);
    }
    for (int slot = 0; slot < SlotCount; ++slot) {
        if (m_ackSlots[slot]) acknowledge(slot);
    }
    auto *node = static_cast<SkwdFrameNode *>(oldNode);
    std::vector<int> pendingSlots;
    quint64 generation;
    bool hasCpuFrame;
    {
        QMutexLocker lock(&m_frameMutex);
        generation = m_generation;
        pendingSlots.swap(m_pendingSlots);
        hasCpuFrame = !m_frame.isEmpty();
    }
    const int pending = pendingSlots.empty() ? -1 : pendingSlots.back();
    if (node && node->streamGeneration != m_streamGeneration && (pending >= 0 || hasCpuFrame)) {
        delete node;
        node = nullptr;
    }
    if (!node && pending < 0 && !hasCpuFrame) {
        return nullptr;
    }
    if (!node) {
        node = new SkwdFrameNode;
        node->streamGeneration = m_streamGeneration;
        node->setOwnsTexture(false);
        node->setFiltering(QSGTexture::Linear);
    }
    node->setRect(boundingRect());
    bool retiring = false;
    if (node->streamGeneration == m_streamGeneration) {
        for (int slot = 0; slot < SlotCount; ++slot) {
            if (!node->textures[slot].retired) continue;
            const GLenum status = node->retirementStatus(slot);
            if (status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED) {
                acknowledge(slot);
            } else if (status == GL_WAIT_FAILED || node->textures[slot].retirementAge.elapsed() >= 2000) {
                frameFailed(QStringLiteral("Wallpaper GPU frame release did not complete"));
                return presentable(node);
            } else {
                retiring = true;
            }
        }
    }
    if (retiring && (!m_lateAcks || slotsExhausted())) scheduleFramePoll();
    if (pending >= 0) {
        for (int slot : pendingSlots) {
            bool imported = false;
            if (!m_gpuUnavailable) {
                QMutexLocker lock(&m_frameMutex);
                imported = node->import(slot, m_slots[slot], window());
            }
            const bool waited = imported && node->wait(slot);
            if (waited) continue;
            if (m_stillStream) {
                if (!m_gpuUnavailable) {
                    qWarning() << "skwd-paper-plasma: GPU transition frames unavailable, presenting the still frame only";
                    m_gpuUnavailable = true;
                }
                for (int skipped : pendingSlots) acknowledge(skipped);
                return presentable(node);
            }
            frameFailed(imported ? QStringLiteral("Cannot wait for the wallpaper GPU frame")
                                 : QStringLiteral("Cannot import the wallpaper GPU frame on this graphics device"));
            return presentable(node);
        }
        if (node->currentSlot >= 0 && node->currentSlot != pending) {
            if (!node->retire(node->currentSlot)) {
                frameFailed(QStringLiteral("Cannot track wallpaper GPU frame release"));
                return presentable(node);
            }
            retiring = true;
        }
        for (int slot : pendingSlots) {
            if (slot != pending) {
                if (!node->retire(slot)) {
                    frameFailed(QStringLiteral("Cannot track wallpaper GPU frame release"));
                    return presentable(node);
                }
                retiring = true;
            }
        }
        if (retiring) {
            glFlush();
            if (!m_lateAcks || slotsExhausted()) scheduleFramePoll();
        }
        node->setTexture(node->textures[pending].texture);
        node->setOwnsTexture(false);
        node->currentSlot = pending;
        node->generation = generation;
        frameAccepted();
        return presentable(node);
    }
    QByteArray frame;
    int frameWidth;
    int frameHeight;
    {
        QMutexLocker lock(&m_frameMutex);
        if (generation == node->generation || m_frame.isEmpty()) {
            return presentable(node);
        }
        frame = m_frame;
        frameWidth = m_frameWidth;
        frameHeight = m_frameHeight;
    }
    if (frameWidth <= 0 || frameHeight <= 0
        || frame.size() != qsizetype(frameWidth) * qsizetype(frameHeight) * 4) {
        return presentable(node);
    }
    if (node->currentSlot >= 0) {
        delete node;
        node = new SkwdFrameNode;
        node->streamGeneration = m_streamGeneration;
        node->setFiltering(QSGTexture::Linear);
        node->setRect(boundingRect());
    }
    const QImage borrowed(reinterpret_cast<const uchar *>(frame.constData()), frameWidth,
        frameHeight, frameWidth * 4, QImage::Format_RGBA8888);
    // The scene graph may defer the texture upload until after this function returns. A QImage
    // wrapping QByteArray storage does not keep that storage alive, so detach into owned pixels.
    const QImage image = borrowed.copy();
    if (image.isNull()) {
        return presentable(node);
    }
    QSGTexture *texture = window()->createTextureFromImage(image, QQuickWindow::TextureIsOpaque);
    if (!texture) {
        frameFailed(QStringLiteral("Cannot upload the wallpaper frame"));
        return presentable(node);
    }
    node->setTexture(texture);
    node->setOwnsTexture(true);
    node->generation = generation;
    frameAccepted();
    return presentable(node);
}
