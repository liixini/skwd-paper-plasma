#include "skwdvideoitem.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <signal.h>
#include <cerrno>

class Item : public SkwdVideoItem {
public:
    using SkwdVideoItem::SkwdVideoItem;
    using SkwdVideoItem::componentComplete;
    using SkwdVideoItem::setSharedImageDevice;
};

int main(int argc, char **argv)
{
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    QQuickWindow::setSceneGraphBackend("software");
    QGuiApplication app(argc, argv);
    QQuickWindow window;
    window.resize(32, 32);
    QDir().mkpath(runtime.path() + "/skwd-paper-plasma");
    QFile helper(runtime.path() + "/presenter");
    if (!helper.open(QIODevice::WriteOnly)) return 1;
    helper.write(R"PY(#!/usr/bin/python3
import os, socket, struct, sys, signal, array
mode = sys.argv[sys.argv.index('--assignment') + 1]
if mode.startswith('{'):
    runtime = os.environ['XDG_RUNTIME_DIR']
    specs = [sys.argv[i + 1] for i, arg in enumerate(sys.argv) if arg == '--stream']
    if 'legacy' in mode:
        with open(runtime + '/shared.log', 'a') as log:
            log.write('legacy %s\n' % ' '.join(sys.argv[1:]))
        if specs:
            sys.stderr.write("error: unexpected argument '--stream' found\n")
            sys.exit(2)
        socket.socket(fileno=int(sys.argv[sys.argv.index('--stream-fd') + 1])).send(b'SKDG' + bytes([6]) + bytes(27))
        sys.stdout.buffer.write(b'SKWP' + struct.pack('<II', 16, 16) + bytes([255, 0, 0, 255]) * 256)
        sys.stdout.buffer.flush()
        sys.stdin.read()
        sys.exit(0)
    size = sys.argv[sys.argv.index('--stream-size') + 1] if '--stream-size' in sys.argv else ''
    with open(runtime + '/shared.log', 'a') as log:
        log.write('spawn %d %s %s %s\n' % (os.getpid(), mode, ' '.join(specs), size))
    streams = []
    for spec in specs:
        fields = dict(item.split('=') for item in spec.split(','))
        stream = socket.socket(fileno=int(fields['fd']))
        epoch = 0
        if 'prelude' in mode:
            def packet(kind, epoch, rest=bytes(24)):
                return b'SKDG' + bytes([kind, 0]) + struct.pack('<H', epoch) + rest
            stream.send(packet(7, 1))
            assert stream.recv(32) == packet(8, 1)
            fd = os.open('/dev/null', os.O_RDONLY)
            geometry = struct.pack('<IIIIQ', 16, 16, 64, 0, 0)
            stream.sendmsg([packet(1, 1, geometry)], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', [fd]))])
            os.close(fd)
            stream.send(packet(2, 1))
            stream.settimeout(2)
            try:
                acked = stream.recv(32) == packet(3, 1)
            except TimeoutError:
                acked = False
            with open(runtime + '/shared.log', 'a') as log:
                log.write('prelude %s\n' % ('acked' if acked else 'starved'))
            stream.settimeout(None)
            stream.send(packet(7, 2))
            assert stream.recv(32) == packet(8, 2)
            epoch = 2
        if 'frame_fd' in fields:
            os.write(int(fields['frame_fd']), b'SKWP' + struct.pack('<II', 16, 16) + bytes([0, 255, 0, 255]) * 256)
        stream.send(b'SKDG' + bytes([6, 0]) + struct.pack('<H', epoch) + bytes(24))
        streams.append(stream)
    for line in sys.stdin:
        with open(runtime + '/shared.log', 'a') as log:
            log.write('control ' + line)
    sys.exit(0)
if mode == 'stubborn':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    with open(os.environ['XDG_RUNTIME_DIR'] + '/stubborn.pid', 'w') as pid:
        pid.write(str(os.getpid()))
if mode == 'fdspam':
    stream = socket.socket(fileno=3)
    fd = os.open('/dev/null', os.O_RDONLY)
    for _ in range(100):
        for data, fds in [(b'bad', [fd]), (b'SKDG' + bytes([4, 9]) + bytes(26), [fd]),
                          (b'SKDG' + bytes([4, 0, 1]) + bytes(25), [fd]),
                          (b'SKDG' + bytes([4, 0]) + bytes(26), [fd, fd, fd])]:
            stream.sendmsg([data], [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array('i', fds))])
    os.close(fd)
    stream.send(b'SKDG' + bytes([6]) + bytes(27))
if mode == 'duplicate':
    stream = socket.socket(fileno=3)
    for _ in range(2):
        stream.send(b'SKDG' + bytes([2, 0]) + bytes(26))
    sys.stdin.read()
    sys.exit(0)
if mode == 'burst':
    stream = socket.socket(fileno=3)
    for slot in range(3):
        stream.send(b'SKDG' + bytes([2, slot]) + bytes(26))
    stream.settimeout(0.2)
    try:
        stream.recv(32)
        sys.stderr.write('acknowledged before rendering\n')
    except TimeoutError:
        sys.stderr.write('held until rendering\n')
    sys.exit(2)
if mode in ('epochs', 'stale'):
    stream = socket.socket(fileno=3)
    def packet(kind, epoch):
        return b'SKDG' + bytes([kind, 0]) + struct.pack('<H',epoch) + bytes(24)
    for epoch in (1, 2):
        stream.send(packet(7, epoch))
        assert stream.recv(32) == packet(8, epoch)
    stream.send(packet(6, 1 if mode == 'stale' else 2))
if mode == 'fail':
    sys.stderr.write('synthetic export unavailable\n')
    sys.exit(2)
if mode in ('ready', 'stubborn'):
    socket.socket(fileno=3).send(b'SKDG' + bytes([6]) + bytes(27))
sys.stdout.buffer.write(b'SKWP' + struct.pack('<II',16,16) + bytes([255,0,0,255])*256)
sys.stdout.buffer.flush()
if mode == 'stale':
    sys.exit(0)
if mode == 'prelude':
    sys.stderr.write('failed after transition prelude\n')
    sys.exit(3)
sys.stdin.read()
)PY");
    helper.close();
    helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Item item(window.contentItem());
    item.setSize(QSizeF(32,32));
    item.setPaper(helper.fileName());
    item.componentComplete();
    int request = 0;
    auto check = [&](const QString &assignment, const QString &expected, const QString &detail = QString()) {
        const QString id = QStringLiteral("123-1-%1").arg(++request);
        QFile file(runtime.path() + "/skwd-paper-plasma/" + id + ".json");
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write("{\"state\":\"pending\"}");
        file.close();
        item.setPresentationId(id);
        item.setAssignment(assignment);
        QElapsedTimer timer;
        timer.start();
        QJsonObject status;
        while (timer.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (file.open(QIODevice::ReadOnly)) {
                status = QJsonDocument::fromJson(file.readAll()).object();
                file.close();
                if (status["state"] == expected) break;
            }
            QThread::msleep(5);
        }
        const bool ok = status["state"] == expected && status["error"].toString().contains(detail);
        if (!ok) std::cerr << "presentation " << assignment.toStdString() << ": " << QJsonDocument(status).toJson().toStdString();
        return ok;
    };
  
    item.setVisible(false);
    if (!check("burst", "error", "held until rendering")) return 7;
    if (!check("duplicate", "error", "reused a GPU frame before release")) return 29;
    item.setVisible(true);
    window.show();
    if (!check("ready", "ready")) return 2;
    if (!check("ready", "ready")) return 3;
    const int descriptors = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
    if (!check("fdspam", "ready")) return 30;
    const int afterSpam = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
    if (afterSpam > descriptors + 4) {
        std::cerr << "rejected packets leaked descriptors: " << descriptors << " -> " << afterSpam << std::endl;
        return 31;
    }
    if (!check("epochs", "ready")) return 8;
    if (!check("stale", "error", "Wallpaper renderer exited")) return 9;
    if (!check("fail", "error", "synthetic export unavailable")) return 4;
    if (!check("prelude", "error", "failed after transition prelude")) return 5;
    item.setPaper(runtime.path() + "/missing");
    if (!check("missing", "error", "Cannot start")) return 6;

    Item left(window.contentItem());
    Item right(window.contentItem());
    for (auto *shared : {&left, &right}) {
        shared->setSize(QSizeF(32, 32));
        shared->setPaper(helper.fileName());
        shared->setSharedImageDevice("device", "driver");
    }
    left.setOutput(QStringLiteral("DP-1"));
    right.setOutput(QStringLiteral("DP-2"));
    right.setStreamWidth(640);
    right.setStreamHeight(480);
    left.componentComplete();
    right.componentComplete();
    left.setAssignment(QStringLiteral(R"({"outputs":["DP-1"],"source":{"kind":"video","path":"/wall/loop.mp4"}})"));
    right.setAssignment(QStringLiteral(R"({"outputs":["DP-2"],"source":{"kind":"video","path":"/wall/loop.mp4"}})"));
    auto settled = [&](auto predicate) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 3000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (predicate()) return true;
            QThread::msleep(5);
        }
        return false;
    };
    auto sharedLog = [&] {
        QFile log(runtime.path() + "/shared.log");
        return log.open(QIODevice::ReadOnly) ? QString::fromUtf8(log.readAll()) : QString();
    };
    if (!settled([&] { return left.workerReady() && right.workerReady(); })) {
        std::cerr << "shared workers never became ready: " << sharedLog().toStdString();
        return 10;
    }
    if (!left.sharesWorker() || !right.sharesWorker()) return 11;
    QString log = sharedLog();
    const auto spawns = log.split('\n').filter(QStringLiteral("spawn "));
    if (spawns.size() != 1) {
        std::cerr << "expected one shared spawn: " << log.toStdString();
        return 12;
    }
    if (!spawns.first().contains("fd=3,size=1280x720,fps=30,output=DP-1,paused=0")
        || !spawns.first().contains("fd=4,size=640x480,fps=30,output=DP-2,paused=0")
        || !spawns.first().contains("\"outputs\":[\"DP-1\",\"DP-2\"]")) {
        std::cerr << "unexpected shared spawn: " << spawns.first().toStdString();
        return 13;
    }
    left.setAssignment(QStringLiteral(R"({"mute":true,"outputs":["DP-1"],"source":{"kind":"video","path":"/wall/loop.mp4"},"transition":{"duration_ms":2000,"effect":"fade","from":"/wall/old.png"},"volume":35})"));
    if (!settled([&] { return sharedLog().contains("control {\"mute\":true,\"to\":\"DP-1\",\"volume\":35}"); })) {
        std::cerr << "audio-only change did not reach the running presenter: " << sharedLog().toStdString();
        return 39;
    }
    if (sharedLog().split('\n').filter(QStringLiteral("spawn ")).size() != 1 || !left.workerReady()) {
        std::cerr << "audio-only change restarted the presenter: " << sharedLog().toStdString();
        return 40;
    }
    right.setPaused(true);
    if (!settled([&] { return sharedLog().contains("control {\"pause\":true,\"to\":\"DP-2\"}"); })) {
        std::cerr << "routed pause missing: " << sharedLog().toStdString();
        return 14;
    }
    right.setAssignment(QStringLiteral(R"({"outputs":["DP-2"],"source":{"kind":"video","path":"/wall/other.mp4"}})"));
    if (!settled([&] { return sharedLog().split('\n').filter(QStringLiteral("spawn ")).size() == 3; })) {
        std::cerr << "split did not respawn both presenters: " << sharedLog().toStdString();
        return 15;
    }
    if (!settled([&] { return left.workerReady() && right.workerReady(); })) return 16;
    const auto respawns = sharedLog().split('\n').filter(QStringLiteral("spawn "));
    if (!respawns.last().contains("other.mp4") && !respawns.at(1).contains("other.mp4")) return 17;
    Item still(window.contentItem());
    Item stillTwo(window.contentItem());
    for (auto *shared : {&still, &stillTwo}) {
        shared->setSize(QSizeF(32, 32));
        shared->setPaper(helper.fileName());
        shared->componentComplete();
    }
    still.setOutput(QStringLiteral("DP-3"));
    stillTwo.setOutput(QStringLiteral("DP-4"));
    const QString stillId = QStringLiteral("123-2-1");
    const QString stillTwoId = QStringLiteral("123-2-2");
    for (const auto &pending : {stillId, stillTwoId}) {
        QFile file(runtime.path() + "/skwd-paper-plasma/" + pending + ".json");
        if (!file.open(QIODevice::WriteOnly)) return 21;
        file.write("{\"state\":\"pending\"}");
    }
    still.setPresentationId(stillId);
    stillTwo.setPresentationId(stillTwoId);
    still.setAssignment(QStringLiteral(R"({"outputs":["DP-3"],"source":{"kind":"static","path":"/wall/a.png"}})"));
    stillTwo.setAssignment(QStringLiteral(R"({"outputs":["DP-4"],"source":{"kind":"static","path":"/wall/a.png"}})"));
    auto reported = [&](const QString &id) {
        QFile file(runtime.path() + "/skwd-paper-plasma/" + id + ".json");
        if (!file.open(QIODevice::ReadOnly)) return false;
        return QJsonDocument::fromJson(file.readAll()).object()["state"] == "ready";
    };
    if (!settled([&] { return reported(stillId) && reported(stillTwoId); })) {
        std::cerr << "shared stills never presented: " << sharedLog().toStdString();
        return 22;
    }
    if (!still.sharesWorker() || !stillTwo.sharesWorker()) return 18;
    const auto stillSpawns = sharedLog().split('\n').filter(QStringLiteral("a.png"));
    if (stillSpawns.size() != 1 || !stillSpawns.first().contains("frame_fd=4") || !stillSpawns.first().contains("frame_fd=6")) {
        std::cerr << "unexpected still spawn: " << sharedLog().toStdString();
        return 23;
    }
    Item transitioned(window.contentItem());
    transitioned.setSize(QSizeF(32, 32));
    transitioned.setPaper(helper.fileName());
    transitioned.setSharedImageDevice("device", "driver");
    transitioned.setOutput(QStringLiteral("DP-7"));
    transitioned.componentComplete();
    const QString transitionedId = QStringLiteral("123-2-3");
    {
        QFile file(runtime.path() + "/skwd-paper-plasma/" + transitionedId + ".json");
        if (!file.open(QIODevice::WriteOnly)) return 36;
        file.write("{\"state\":\"pending\"}");
    }
    transitioned.setPresentationId(transitionedId);
    transitioned.setAssignment(QStringLiteral(R"({"outputs":["DP-7"],"source":{"kind":"static","path":"/wall/prelude.png"}})"));
    if (!settled([&] { return reported(transitionedId) && sharedLog().contains("prelude "); })) {
        std::cerr << "still behind an unusable GPU prelude never presented: " << sharedLog().toStdString();
        return 37;
    }
    if (!sharedLog().contains("prelude acked")) {
        std::cerr << "skipped GPU prelude frame starved the renderer: " << sharedLog().toStdString();
        return 38;
    }
    still.setAssignment(QStringLiteral(R"({"outputs":["DP-3"],"source":{"kind":"video","path":"/wall/a.ivf","engine":"tinier","frame_rate":"30/1"}})"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (still.sharesWorker()) return 19;
    still.setAssignment(QStringLiteral(R"({"outputs":["DP-3"],"source":{"kind":"video","path":"/wall/a.mp4","engine":"default"}})"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (still.sharesWorker()) return 24;
    still.setSharedImageDevice("device", "driver");
    still.setAssignment(QStringLiteral(R"({"outputs":["DP-3"],"source":{"kind":"video","path":"/wall/b.mp4","engine":"default"}})"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!still.sharesWorker()) return 20;
    QFile legacyHelper(runtime.path() + "/legacy-presenter");
    if (!legacyHelper.open(QIODevice::WriteOnly)) return 25;
    QFile source(helper.fileName());
    if (!source.open(QIODevice::ReadOnly)) return 28;
    legacyHelper.write(source.readAll());
    legacyHelper.close();
    legacyHelper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    Item oldLeft(window.contentItem());
    Item oldRight(window.contentItem());
    for (auto *shared : {&oldLeft, &oldRight}) {
        shared->setSize(QSizeF(32, 32));
        shared->setPaper(legacyHelper.fileName());
        shared->setSharedImageDevice("device", "driver");
        shared->componentComplete();
    }
    oldLeft.setOutput(QStringLiteral("DP-5"));
    oldRight.setOutput(QStringLiteral("DP-6"));
    oldLeft.setAssignment(QStringLiteral(R"({"outputs":["DP-5"],"source":{"kind":"video","path":"/wall/legacy.mp4"}})"));
    oldRight.setAssignment(QStringLiteral(R"({"outputs":["DP-6"],"source":{"kind":"video","path":"/wall/legacy.mp4"}})"));
    if (!settled([&] { return oldLeft.workerReady() && oldRight.workerReady() && !oldLeft.sharesWorker() && !oldRight.sharesWorker(); })) {
        std::cerr << "legacy fallback did not recover: " << sharedLog().toStdString();
        return 26;
    }
    const auto legacyRuns = sharedLog().split('\n').filter(QStringLiteral("legacy "));
    if (legacyRuns.size() != 3 || !legacyRuns.first().contains("--stream fd=")
        || !legacyRuns.at(1).contains("--stream-fd 3") || legacyRuns.at(1).contains("--stream fd=")) {
        std::cerr << "unexpected legacy sequence: " << sharedLog().toStdString();
        return 27;
    }
    Item lockWide(window.contentItem());
    Item lockPortrait(window.contentItem());
    for (auto *shared : {&lockWide, &lockPortrait}) {
        shared->setSize(QSizeF(32, 32));
        shared->setPaper(helper.fileName());
        shared->componentComplete();
    }
    lockWide.setOutput(QStringLiteral("DP-8"));
    lockPortrait.setOutput(QStringLiteral("DP-9"));
    lockWide.setStreamWidth(2560);
    lockWide.setStreamHeight(1440);
    lockPortrait.setStreamWidth(2162);
    lockPortrait.setStreamHeight(3841);
    lockWide.setAssignment(QStringLiteral(R"({"outputs":["*"],"source":{"kind":"static","path":"/wall/lock.png"}})"));
    lockPortrait.setAssignment(QStringLiteral(R"({"outputs":["*"],"source":{"kind":"static","path":"/wall/lock.png"}})"));
    if (!settled([&] { return sharedLog().contains("lock.png"); })) {
        std::cerr << "shared lock-screen still never spawned: " << sharedLog().toStdString();
        return 41;
    }
    const auto lockSpawns = sharedLog().split('\n').filter(QStringLiteral("lock.png"));
    if (lockSpawns.size() != 1 || !lockSpawns.first().contains("size=2560x1440,fps=30,output=DP-8")
        || !lockSpawns.first().contains("size=2160x3838,fps=30,output=DP-9")) {
        std::cerr << "still streams must fit the 3840x2160 renderer budget: " << sharedLog().toStdString();
        return 42;
    }
    Item portraitVideo(window.contentItem());
    portraitVideo.setSize(QSizeF(32, 32));
    portraitVideo.setPaper(helper.fileName());
    portraitVideo.setSharedImageDevice("device", "driver");
    portraitVideo.setOutput(QStringLiteral("DP-10"));
    portraitVideo.componentComplete();
    portraitVideo.setStreamWidth(2880);
    portraitVideo.setStreamHeight(5120);
    portraitVideo.setAssignment(QStringLiteral(R"({"outputs":["DP-10"],"source":{"kind":"video","path":"/wall/portrait.mp4"}})"));
    if (!settled([&] { return sharedLog().contains("portrait.mp4"); })) {
        std::cerr << "portrait video never spawned: " << sharedLog().toStdString();
        return 43;
    }
    if (!sharedLog().contains("size=2880x5120,fps=30,output=DP-10")) {
        std::cerr << "GPU video streams keep the portrait output size: " << sharedLog().toStdString();
        return 44;
    }
    Item tinierWide(window.contentItem());
    tinierWide.setSize(QSizeF(32, 32));
    tinierWide.setPaper(helper.fileName());
    tinierWide.setOutput(QStringLiteral("DP-11"));
    tinierWide.componentComplete();
    tinierWide.setStreamWidth(5120);
    tinierWide.setStreamHeight(2880);
    tinierWide.setAssignment(QStringLiteral(R"({"outputs":["DP-11"],"source":{"kind":"video","path":"/wall/wide.ivf","engine":"tinier","frame_rate":"30/1"}})"));
    if (!settled([&] { return sharedLog().contains("wide.ivf"); })) {
        std::cerr << "tinier video never spawned: " << sharedLog().toStdString();
        return 45;
    }
    if (tinierWide.sharesWorker() || !sharedLog().split('\n').filter(QStringLiteral("wide.ivf")).first().endsWith(" 3840x2160")) {
        std::cerr << "direct tinier streams must fit the renderer budget: " << sharedLog().toStdString();
        return 46;
    }
    auto *stubborn = new Item(window.contentItem());
    stubborn->setSize(QSizeF(32, 32));
    stubborn->setPaper(helper.fileName());
    stubborn->setAssignment("stubborn");
    stubborn->componentComplete();
    if (!settled([&] { return stubborn->workerReady(); })) return 32;
    QFile pidFile(runtime.path() + "/stubborn.pid");
    if (!pidFile.open(QIODevice::ReadOnly)) return 33;
    const int pid = pidFile.readAll().trimmed().toInt();
    QElapsedTimer removal;
    removal.start();
    delete stubborn;
    const auto removalMs = removal.elapsed();
    std::cout << "unresponsive renderer removal: " << removalMs << " ms" << std::endl;
    if (removalMs >= 100) return 34;
    if (!settled([&] { return ::kill(pid, 0) == -1 && errno == ESRCH; })) return 35;
    return 0;
}
