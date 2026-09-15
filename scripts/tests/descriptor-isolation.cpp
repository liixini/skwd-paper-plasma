#include "skwdvideoitem.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QThread>
#include <QStringList>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

class Item : public SkwdVideoItem {
public:
    using SkwdVideoItem::SkwdVideoItem;
    using SkwdVideoItem::componentComplete;
    using SkwdVideoItem::setSharedImageDevice;
};

int presenter(int argc, char **argv)
{
    std::array<char, 32> ready {};
    std::memcpy(ready.data(), "SKDG", 4);
    ready[4] = 6;
    bool sent = false;
    for (int i = 1; i + 1 < argc; ++i) {
        int fd = -1;
        if (std::strcmp(argv[i], "--stream-fd") == 0) fd = QString::fromLocal8Bit(argv[i + 1]).toInt();
        if (std::strcmp(argv[i], "--stream") == 0) {
            const auto fields = QString::fromLocal8Bit(argv[i + 1]).split(',');
            for (const auto &field : fields) {
                if (field.startsWith("fd=")) fd = field.mid(3).toInt();
            }
        }
        if (fd >= 0) {
            if (::send(fd, ready.data(), ready.size(), MSG_NOSIGNAL) != ssize_t(ready.size())) return 2;
            sent = true;
        }
    }
    if (!sent) return 3;
    char byte;
    while (::read(STDIN_FILENO, &byte, 1) > 0) {}
    return 0;
}

bool settled(auto predicate)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) return true;
        QThread::msleep(2);
    }
    return false;
}

bool check(QQuickWindow &window, bool shared)
{
    int pipes[2];
    if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    const int inheritedWriter = ::fcntl(pipes[1], F_DUPFD, 128);
    ::close(pipes[1]);
    if (inheritedWriter < 0) { ::close(pipes[0]); return false; }
    bool ready;
    bool eof;
    {
        Item item(window.contentItem());
        item.setSize(QSizeF(16, 16));
        item.setPaper(QCoreApplication::applicationFilePath());
        item.setSharedImageDevice(shared ? QByteArray("device") : QByteArray(), {});
        item.setOutput("DP-test");
        item.setAssignment(shared
            ? QStringLiteral(R"({"source":{"kind":"video","path":"/fixture/video.mp4"}})")
            : QStringLiteral("private-presenter"));
        item.componentComplete();
        ready = settled([&] { return item.workerReady(); });
        ready = ready && item.sharesWorker() == shared;
        ::close(inheritedWriter);
        pollfd descriptor {pipes[0], POLLIN, 0};
        const int result = ::poll(&descriptor, 1, 30);
        char byte;
        eof = result == 1 && (descriptor.revents & POLLHUP) && ::read(pipes[0], &byte, 1) == 0;
        std::cout << (shared ? "shared" : "private") << " renderer ready=" << ready
                  << " unrelated pipe EOF while renderer alive=" << eof << std::endl;
    }
    ::close(pipes[0]);
    return ready && eof;
}

bool checkStartError(QQuickWindow &window, const QString &runtime, const QString &paper, bool shared)
{
    const QString id = shared ? QStringLiteral("1-1-2") : QStringLiteral("1-1-1");
    QFile file(runtime + "/skwd-paper-plasma/" + id + ".json");
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write("{\"state\":\"pending\"}");
    file.close();
    Item item(window.contentItem());
    item.setSize(QSizeF(16, 16));
    item.setPaper(paper);
    item.setPresentationId(id);
    item.setSharedImageDevice(shared ? QByteArray("device") : QByteArray(), {});
    item.setOutput("DP-test");
    item.setAssignment(shared
        ? QStringLiteral(R"({"source":{"kind":"video","path":"/fixture/video.mp4"}})")
        : QStringLiteral("private-presenter"));
    item.componentComplete();
    QJsonObject status;
    const bool reported = settled([&] {
        if (!file.open(QIODevice::ReadOnly)) return false;
        status = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        return status["state"] == "error";
    });
    const bool ok = reported && status["error"].toString().startsWith("Cannot start ")
        && status["error"].toString().contains(paper) && !item.workerReady();
    std::cout << (shared ? "shared" : "private") << " startup error=" << ok
              << " " << QJsonDocument(status).toJson(QJsonDocument::Compact).toStdString() << std::endl;
    return ok;
}

bool disableCloseRange()
{
    sock_filter filter[] {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_close_range, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program {4, filter};
    return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0
        && ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "present-plasma") == 0) return presenter(argc, argv);
    if (argc > 1 && std::strcmp(argv[1], "--no-close-range") == 0 && !disableCloseRange()) return 2;
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    QQuickWindow::setSceneGraphBackend("software");
    QGuiApplication app(argc, argv);
    QQuickWindow window;
    window.resize(32, 32);
    window.show();
    const bool privateOk = check(window, false);
    const bool sharedOk = check(window, true);
    if (!QDir().mkpath(runtime.path() + "/skwd-paper-plasma")) return 3;
    QFile broken(runtime.path() + "/broken-interpreter");
    if (!broken.open(QIODevice::WriteOnly)) return 4;
    broken.write("#!/skwd-test-missing-interpreter\n");
    broken.close();
    if (!broken.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) return 5;
    bool errorsOk = true;
    for (const auto &paper : {runtime.path() + "/missing", broken.fileName()}) {
        for (const bool shared : {false, true}) {
            errorsOk = checkStartError(window, runtime.path(), paper, shared) && errorsOk;
        }
    }
    return privateOk && sharedOk && errorsOk ? 0 : 1;
}
