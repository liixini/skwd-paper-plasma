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
#include <QJsonArray>
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
import os, socket, struct, sys, signal, array, json
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
    with open(runtime + '/events.jsonl', 'a') as log:
        log.write(json.dumps({'kind': 'spawn', 'pid': os.getpid(), 'assignment': json.loads(mode)}) + '\n')
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
        with open(runtime + '/events.jsonl', 'a') as log:
            log.write(json.dumps({'kind': 'control', 'pid': os.getpid(), 'command': json.loads(line)}) + '\n')
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
if mode in ('ready', 'stubborn', 'pattern', 'changed-pattern'):
    socket.socket(fileno=3).send(b'SKDG' + bytes([6]) + bytes(27))
pixels = bytes([255,0,0,255])*256
if mode in ('pattern', 'changed-pattern'):
    left, right = ((255, 0, 0, 255), (0, 0, 255, 255)) if mode == 'pattern' else ((0, 255, 0, 255), (255, 255, 0, 255))
    pixels = (bytes(left) * 8 + bytes(right) * 8) * 16
sys.stdout.buffer.write(b'SKWP' + struct.pack('<II',16,16) + pixels)
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
    auto visiblePixels = [&](const QString &name, const QColor &left, const QColor &right) {
        const auto frame = window.grabWindow();
        const bool correct = !frame.isNull() && frame.pixelColor(8, 16) == left && frame.pixelColor(24, 16) == right;
        const QString artifacts = qEnvironmentVariable("SKWD_TEST_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) {
            QDir().mkpath(artifacts);
            if (!frame.save(artifacts + "/" + name + ".png")) return false;
        }
        if (!correct) std::cerr << "visible pixels did not match " << name.toStdString() << std::endl;
        return correct;
    };
    if (!visiblePixels("red", Qt::red, Qt::red)) return 66;
    if (!check("pattern", "ready") || !visiblePixels("pattern", Qt::red, Qt::blue)) return 67;
    if (!check("changed-pattern", "ready") || !visiblePixels("changed-pattern", Qt::green, Qt::yellow)) return 68;
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
    auto events = [&] {
        QFile file(runtime.path() + "/events.jsonl");
        QList<QJsonObject> values;
        if (file.open(QIODevice::ReadOnly)) {
            for (const auto &line : file.readAll().split('\n')) {
                const auto value = QJsonDocument::fromJson(line).object();
                if (!value.isEmpty()) values.append(value);
            }
        }
        return values;
    };
    auto spawnCount = [&] {
        int count = 0;
        for (const auto &event : events()) count += event["kind"] == "spawn";
        return count;
    };
    auto lastSpawn = [&](const QString &output) {
        QJsonObject latest;
        for (const auto &event : events()) {
            if (event["kind"] == "spawn"
                && event["assignment"].toObject()["outputs"].toArray().contains(output)) latest = event;
        }
        return latest;
    };
    auto audioReceived = [&](int pid, bool mute, int volume, qsizetype after = 0) {
        for (const auto &event : events().mid(after)) {
            const auto command = event["command"].toObject();
            if (event["kind"] == "control" && event["pid"].toInt() == pid
                && command["to"].isString() && command["to"].toString().isEmpty()
                && command["mute"] == mute && command["volume"] == volume) return true;
        }
        return false;
    };
    auto assignment = [](const QString &output, const QString &path, bool mute, int volume, bool transition = false) {
        QJsonObject value {{"outputs", QJsonArray {output}},
                           {"source", QJsonObject {{"kind", "video"}, {"path", path}}},
                           {"mute", mute}, {"volume", volume}};
        if (transition) value["transition"] = QJsonObject {{"duration_ms", 2000}, {"effect", "fade"}, {"from", "/wall/old.png"}};
        return QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact));
    };
    left.setAssignment(assignment("DP-1", "/wall/loop.mp4", true, 35));
    if (!settled([&] { return spawnCount() == 3 && left.workerReady() && right.workerReady(); })) {
        std::cerr << "divergent audio settings did not split presenters: " << sharedLog().toStdString();
        return 39;
    }
    const auto leftSplit = lastSpawn("DP-1");
    const auto rightSplit = lastSpawn("DP-2");
    if (leftSplit["pid"] == rightSplit["pid"]
        || leftSplit["assignment"].toObject()["mute"] != true
        || leftSplit["assignment"].toObject()["volume"] != 35
        || rightSplit["assignment"].toObject().contains("mute")
        || rightSplit["assignment"].toObject().contains("volume")) {
        std::cerr << "audio split changed the other output's assignment: " << sharedLog().toStdString();
        return 40;
    }
    right.setAssignment(assignment("DP-2", "/wall/loop.mp4", true, 35));
    if (!settled([&] { return spawnCount() == 4 && left.workerReady() && right.workerReady(); })) return 47;
    if (lastSpawn("DP-1")["pid"] != lastSpawn("DP-2")["pid"]
        || lastSpawn("DP-1")["assignment"].toObject()["outputs"].toArray().size() != 2) {
        std::cerr << "matching audio settings did not rejoin presenters: " << sharedLog().toStdString();
        return 48;
    }
    right.setPaused(true);
    if (!settled([&] { return sharedLog().contains("control {\"pause\":true,\"to\":\"DP-2\"}"); })) {
        std::cerr << "routed pause missing: " << sharedLog().toStdString();
        return 14;
    }
    right.setAssignment(QStringLiteral(R"({"outputs":["DP-2"],"source":{"kind":"video","path":"/wall/other.mp4"}})"));
    if (!settled([&] { return spawnCount() == 6; })) {
        std::cerr << "split did not respawn both presenters: " << sharedLog().toStdString();
        return 15;
    }
    if (!settled([&] { return left.workerReady() && right.workerReady(); })) return 16;
    if (lastSpawn("DP-2")["assignment"].toObject()["source"].toObject()["path"] != "/wall/other.mp4"
        || lastSpawn("DP-1")["assignment"].toObject()["source"].toObject()["path"] != "/wall/loop.mp4") return 17;
    {
        Item solo(window.contentItem());
        auto configure = [&](Item &target, const QString &output) {
            target.setSize(QSizeF(32, 32));
            target.setPaper(helper.fileName());
            target.setSharedImageDevice("device", "driver");
            target.setOutput(output);
            target.componentComplete();
        };
        configure(solo, "RETUNE-1");
        solo.setAssignment(assignment("RETUNE-1", "/wall/retune.mp4", false, 80));
        if (!settled([&] { return solo.workerReady(); })) return 49;
        const int originalPid = lastSpawn("RETUNE-1")["pid"].toInt();
        const int originalSpawns = spawnCount();
        solo.setAssignment(assignment("RETUNE-1", "/wall/retune.mp4", true, 35));
        if (!settled([&] { return audioReceived(originalPid, true, 35); })) {
            std::cerr << "lone audio retune did not send an audio command: " << sharedLog().toStdString();
            return 50;
        }
        if (spawnCount() != originalSpawns || !solo.workerReady()) return 51;
        QJsonObject omitted = QJsonDocument::fromJson(assignment("RETUNE-1", "/wall/retune.mp4", true, 80).toUtf8()).object();
        omitted.remove("mute");
        omitted.remove("volume");
        solo.setAssignment(QString::fromUtf8(QJsonDocument(omitted).toJson(QJsonDocument::Compact)));
        if (!settled([&] { return audioReceived(originalPid, true, 80); })) {
            std::cerr << "omitted audio settings did not restore Paper defaults: " << sharedLog().toStdString();
            return 62;
        }
        omitted["mute"] = false;
        solo.setAssignment(QString::fromUtf8(QJsonDocument(omitted).toJson(QJsonDocument::Compact)));
        if (!settled([&] { return audioReceived(originalPid, false, 80); })) return 63;
        const auto beforeExplicit = events().size();
        solo.setAssignment(assignment("RETUNE-1", "/wall/retune.mp4", true, 35));
        if (!settled([&] { return audioReceived(originalPid, true, 35, beforeExplicit); })) return 64;
        if (spawnCount() != originalSpawns || !solo.workerReady()) return 65;
        Item oldSettings(window.contentItem());
        configure(oldSettings, "RETUNE-2");
        oldSettings.setAssignment(assignment("RETUNE-2", "/wall/retune.mp4", false, 80));
        if (!settled([&] { return oldSettings.workerReady(); })) return 52;
        if (spawnCount() != originalSpawns + 1 || lastSpawn("RETUNE-1")["pid"] != originalPid
            || lastSpawn("RETUNE-2")["pid"] == originalPid) {
            std::cerr << "new output joined a stale audio key: " << sharedLog().toStdString();
            return 53;
        }
        {
            Item matching(window.contentItem());
            configure(matching, "RETUNE-3");
            matching.setAssignment(assignment("RETUNE-3", "/wall/retune.mp4", true, 35));
            if (!settled([&] { return matching.workerReady() && solo.workerReady(); })) return 54;
            if (lastSpawn("RETUNE-1")["pid"] != lastSpawn("RETUNE-3")["pid"]
                || lastSpawn("RETUNE-1")["pid"] == lastSpawn("RETUNE-2")["pid"]) return 55;
        }
        const int afterDetach = spawnCount();
        if (!settled([&] { return spawnCount() == afterDetach + 1 && solo.workerReady(); })) return 56;
        const int transitionPid = lastSpawn("RETUNE-1")["pid"].toInt();
        const int beforeTransition = spawnCount();
        const auto transitionAssignment = assignment("RETUNE-1", "/wall/retune.mp4", true, 35, true);
        solo.setAssignment(transitionAssignment);
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (spawnCount() != beforeTransition || !solo.workerReady()) return 57;
        {
            Item transitioned(window.contentItem());
            configure(transitioned, "RETUNE-4");
            transitioned.setAssignment(assignment("RETUNE-4", "/wall/retune.mp4", true, 35, true));
            if (!settled([&] { return transitioned.workerReady() && solo.workerReady(); })) return 58;
            if (lastSpawn("RETUNE-1")["pid"] != lastSpawn("RETUNE-4")["pid"]
                || lastSpawn("RETUNE-1")["pid"] == transitionPid) {
                std::cerr << "transition-only retune left a stale pool key: " << sharedLog().toStdString();
                return 59;
            }
        }
        const int pendingSpawns = spawnCount();
        solo.setAssignment(assignment("RETUNE-1", "/wall/retune.mp4", false, 62, true));
        if (!settled([&] { return spawnCount() == pendingSpawns + 1 && solo.workerReady(); })) {
            std::cerr << "retune lost a queued presenter restart: " << sharedLog().toStdString();
            return 60;
        }
        const auto pendingAssignment = lastSpawn("RETUNE-1")["assignment"].toObject();
        if (pendingAssignment["mute"] != false || pendingAssignment["volume"] != 62) return 61;
    }
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
