#include "skwdvideoitem.h"

#include <QImage>
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
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int SlotCount = 3;

quint32 read32(const uchar *data)
{
    return qFromLittleEndian<quint32>(data);
}

quint64 read64(const uchar *data)
{
    return qFromLittleEndian<quint64>(data);
}

class SkwdFrameNode final : public QSGSimpleTextureNode {
public:
    struct Texture {
        EGLImageKHR image = EGL_NO_IMAGE_KHR;
        GLuint name = 0;
        GLuint memory = 0;
        GLuint semaphore = 0;
        QSGTexture *texture = nullptr;
    };

    ~SkwdFrameNode() override
    {
        const bool hasContext = QOpenGLContext::currentContext() != nullptr;
        for (auto &slot : textures) {
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
    m_process.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &SkwdVideoItem::consume);
    connect(&m_process, &QProcess::readyReadStandardError, this, &SkwdVideoItem::collectErrors);
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            failPresentation(QStringLiteral("Cannot start %1: %2").arg(m_paper, m_process.errorString()));
        }
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
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
    closeDmabuf();
    m_process.terminate();
    if (!m_process.waitForFinished(500)) {
        m_process.kill();
        m_process.waitForFinished(500);
    }
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
    m_stderr.append(m_process.readAllStandardError());
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
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
    }
}

void SkwdVideoItem::reportPresentation()
{
    if (m_presentationId.isEmpty() || m_presentationId.size() > 100
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
    m_assignment = value;
    emit assignmentChanged();
    if (isComponentComplete()) {
        scheduleRestart();
    }
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
    value = qBound(16, value, 7680);
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
    value = qBound(16, value, 4320);
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
    if (m_process.state() == QProcess::Running && m_process.write(line) < 0) {
        scheduleRestart();
    }
}

void SkwdVideoItem::sendPause()
{
    QJsonObject command;
    command.insert(QStringLiteral("to"), QString());
    command.insert(QStringLiteral("pause"), m_paused);
    sendControl(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
}

void SkwdVideoItem::closeDmabuf()
{
    delete m_socketNotifier;
    m_socketNotifier = nullptr;
    if (m_socket >= 0) {
        ::close(m_socket);
        m_socket = -1;
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
    m_epoch = 0;
    ++m_streamGeneration;
    m_stopping = true;
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(500);
    }
    closeDmabuf();
    m_stopping = false;
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
    if (m_assignment.isEmpty() || m_paper.isEmpty()) {
        update();
        return;
    }
    int sockets[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        failPresentation(QStringLiteral("Cannot create the wallpaper frame socket"));
        return;
    }
    m_socket = sockets[0];
    const int flags = ::fcntl(m_socket, F_GETFL, 0);
    ::fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
    const int childSocket = sockets[1];
    m_process.setChildProcessModifier([childSocket] {
        ::dup2(childSocket, 3);
        if (childSocket != 3) {
            ::close(childSocket);
        }
    });
    m_socketNotifier = new QSocketNotifier(m_socket, QSocketNotifier::Read, this);
    connect(m_socketNotifier, &QSocketNotifier::activated, this, &SkwdVideoItem::consumeDmabuf);
    QStringList arguments {QStringLiteral("present-plasma"),
            QStringLiteral("--assignment"), m_assignment,
            QStringLiteral("--stream-size"),
            QStringLiteral("%1x%2").arg(m_streamWidth).arg(m_streamHeight),
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
    m_process.setProcessEnvironment(environment);
    m_process.start(m_paper, arguments);
    ::close(childSocket);
}

void SkwdVideoItem::consumeDmabuf()
{
    for (;;) {
        std::array<uchar, 32> bytes {};
        std::array<uchar, CMSG_SPACE(sizeof(int))> control {};
        iovec iov {bytes.data(), bytes.size()};
        msghdr message {};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        const ssize_t length = ::recvmsg(m_socket, &message, MSG_DONTWAIT);
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        if (length <= 0) {
            m_socketNotifier->setEnabled(false);
            return;
        }
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
            m_frame.clear();
            m_buffer.clear();
            m_ready = false;
            m_epoch = epoch;
            ++m_streamGeneration;
            m_workerReady = false;
            m_frameAccepted = false;
            bytes[4] = 8;
            ::send(m_socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
            continue;
        }
        if (epoch != m_epoch) continue;
        if (bytes[4] == 6 && length == 32) {
            m_workerReady = true;
            reportPresentation();
            continue;
        }
        const int slotIndex = bytes[5];
        if (slotIndex < 0 || slotIndex >= SlotCount) {
            continue;
        }
        if ((bytes[4] == 1 || bytes[4] == 4 || bytes[4] == 5) && length == 32) {
            int receivedFd = -1;
            for (cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
                    std::memcpy(&receivedFd, CMSG_DATA(header), sizeof(receivedFd));
                    break;
                }
            }
            QMutexLocker lock(&m_frameMutex);
            auto &slot = m_slots[slotIndex];
            if (bytes[4] == 5) {
                slot.semaphoreFd = receivedFd;
                continue;
            }
            slot.fd = receivedFd;
            slot.width = read32(bytes.data() + 8);
            slot.height = read32(bytes.data() + 12);
            slot.stride = read32(bytes.data() + 16);
            slot.offset = read32(bytes.data() + 20);
            slot.modifier = read64(bytes.data() + 24);
            slot.opaque = bytes[4] == 4;
        } else if (bytes[4] == 2) {
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
    ::send(m_socket, bytes.data(), bytes.size(), MSG_NOSIGNAL);
}

void SkwdVideoItem::consume()
{
    m_buffer.append(m_process.readAllStandardOutput());
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
    if (pending >= 0) {
        // A dropped frame still owns a signaled binary semaphore. Consume every
        // signal on the render thread before allowing the producer to reuse its
        // slot; acknowledging a skipped frame from the GUI thread can otherwise
        // cause Vulkan to signal the same semaphore twice and stall the queue.
        for (int slot : pendingSlots) {
            bool imported;
            {
                QMutexLocker lock(&m_frameMutex);
                imported = node->import(slot, m_slots[slot], window());
            }
            if (!imported) {
                frameFailed(QStringLiteral("Cannot import the wallpaper GPU frame on this graphics device"));
                return node;
            }
            if (!node->wait(slot)) {
                frameFailed(QStringLiteral("Cannot wait for the wallpaper GPU frame"));
                return node;
            }
        }
        if (node->currentSlot >= 0 || pendingSlots.size() > 1) {
            glFinish();
            if (node->currentSlot >= 0 && node->currentSlot != pending) {
                acknowledge(node->currentSlot);
            }
            for (int slot : pendingSlots) {
                if (slot != pending) {
                    acknowledge(slot);
                }
            }
        }
        node->setTexture(node->textures[pending].texture);
        node->setOwnsTexture(false);
        node->currentSlot = pending;
        node->generation = generation;
        frameAccepted();
        return node;
    }
    QByteArray frame;
    int frameWidth;
    int frameHeight;
    {
        QMutexLocker lock(&m_frameMutex);
        if (generation == node->generation || m_frame.isEmpty()) {
            return node;
        }
        frame = m_frame;
        frameWidth = m_frameWidth;
        frameHeight = m_frameHeight;
    }
    if (frameWidth <= 0 || frameHeight <= 0
        || frame.size() != qsizetype(frameWidth) * qsizetype(frameHeight) * 4) {
        return node;
    }
    const QImage borrowed(reinterpret_cast<const uchar *>(frame.constData()), frameWidth,
        frameHeight, frameWidth * 4, QImage::Format_RGBA8888);
    // The scene graph may defer the texture upload until after this function returns. A QImage
    // wrapping QByteArray storage does not keep that storage alive, so detach into owned pixels.
    const QImage image = borrowed.copy();
    if (image.isNull()) {
        return node;
    }
    QSGTexture *texture = window()->createTextureFromImage(image, QQuickWindow::TextureIsOpaque);
    if (!texture) {
        frameFailed(QStringLiteral("Cannot upload the wallpaper frame"));
        return node;
    }
    node->setTexture(texture);
    node->setOwnsTexture(true);
    node->generation = generation;
    frameAccepted();
    return node;
}
