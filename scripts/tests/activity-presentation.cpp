#include "skwdvideoitem.h"
#include "skwdworkerpool.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QThread>
#include <QMouseEvent>
#include <QSet>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/resource.h>

static QStringList resourceErrors;

class Item : public SkwdVideoItem {
public:
    using SkwdVideoItem::SkwdVideoItem;
    using SkwdVideoItem::componentComplete;
    using SkwdVideoItem::setSharedImageDevice;
};

static void check(bool ok, const char *label)
{
    if (!ok) throw std::runtime_error(label);
    std::cout << "PASS " << label << std::endl;
}

static bool waitFor(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) return true;
        QThread::msleep(2);
    }
    return false;
}

static QJsonArray events(const QString &runtime)
{
    QFile log(runtime + "/events.jsonl");
    QJsonArray result;
    if (!log.open(QIODevice::ReadOnly)) return result;
    for (const auto &line : log.readAll().split('\n')) {
        const auto value = QJsonDocument::fromJson(line).object();
        if (!value.isEmpty()) result.append(value);
    }
    return result;
}

static QJsonArray spawns(const QString &runtime)
{
    QJsonArray result;
    for (const auto &value : events(runtime)) {
        if (value.toObject()["kind"] == "spawn") result.append(value);
    }
    return result;
}

static QJsonObject lastControl(const QString &runtime, const QString &id, const QString &field)
{
    QJsonObject result;
    for (const auto &value : events(runtime)) {
        const auto event = value.toObject();
        const auto command = event["command"].toObject();
        if (event["kind"] == "control" && command["to"] == id && command.contains(field)) result = command;
    }
    return result;
}

static QString assignment(const QString &output, const QString &path = "/wall/green.png", int volume = 80)
{
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {"outputs", QJsonArray{output}}, {"source", QJsonObject{{"kind", "static"}, {"path", path}}},
        {"mute", true}, {"volume", volume}}).toJson(QJsonDocument::Compact));
}

static void run(const QString &runtime)
{
    QFile helper(runtime + "/presenter");
    check(helper.open(QIODevice::WriteOnly), "create presenter");
    helper.write(R"PY(#!/usr/bin/python3
import json, os, socket, struct, sys
assignment = json.loads(sys.argv[sys.argv.index('--assignment') + 1])
outputs = assignment['outputs']
if len(outputs) != len(set(outputs)):
    sys.stderr.write('output appears in more than one assignment\n')
    sys.exit(1)
streams = [dict(field.split('=', 1) for field in sys.argv[i + 1].split(','))
           for i, arg in enumerate(sys.argv) if arg == '--stream']
labels = [stream['output'] for stream in streams]
if len(labels) != len(set(labels)):
    sys.stderr.write('duplicate stream control address\n')
    sys.exit(1)
def log(value):
    with open(os.environ['XDG_RUNTIME_DIR'] + '/events.jsonl', 'a') as out:
        out.write(json.dumps(dict(pid=os.getpid(), **value)) + '\n')
log(dict(kind='spawn', assignment=assignment, streams=streams))
sockets = []
for fields in streams:
    channel = socket.socket(fileno=int(fields['fd']))
    sockets.append(channel)
    if 'frame_fd' in fields:
        os.write(int(fields['frame_fd']), b'SKWP' + struct.pack('<II', 16, 16) + bytes([0,255,0,255]) * 256)
    channel.send(b'SKDG' + bytes([6]) + bytes(27))
for line in sys.stdin:
    command = json.loads(line)
    if command.get('to') and command['to'] not in labels:
        sys.stderr.write('control addressed unknown stream\n')
        sys.exit(1)
    log(dict(kind='control', command=command))
)PY");
    helper.close();
    helper.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QQuickWindow firstWindow, secondWindow;
    firstWindow.resize(32, 32);
    secondWindow.resize(32, 32);
    firstWindow.show();
    secondWindow.show();
    QQuickItem firstActivity(firstWindow.contentItem()), secondActivity(firstWindow.contentItem());
    firstActivity.setSize(QSizeF(32, 32));
    secondActivity.setSize(QSizeF(32, 32));
    secondActivity.setVisible(false);
    auto make = [&](QQuickItem *parent, const QString &output, const QString &path = "/wall/green.png") {
        auto item = std::make_unique<Item>(parent);
        item->setSize(QSizeF(32, 32));
        item->setStreamWidth(32);
        item->setStreamHeight(32);
        item->setPaper(helper.fileName());
        item->setOutput(output);
        item->setAssignment(assignment(output, path));
        item->componentComplete();
        return item;
    };
    auto a = make(&firstActivity, "DP-2");
    auto b = make(&secondActivity, "DP-2");
    const QString originalBId = b->streamId();
    auto c = make(secondWindow.contentItem(), "DP-3");
    auto ready = [&] { return a->workerReady() && b->workerReady() && c->workerReady(); };
    check(waitFor(ready), "duplicate output streams become ready");
    check(spawns(runtime).size() == 1 && SkwdWorkerPool::instance()->workerCount() == 1,
          "three activity instances share one presenter");
    auto initial = spawns(runtime).last().toObject();
    check(initial["assignment"].toObject()["outputs"].toArray() == QJsonArray{"DP-2", "DP-3"},
          "physical monitor list contains each monitor once");
    QSet<QString> labels;
    for (const auto &stream : initial["streams"].toArray()) labels.insert(stream.toObject()["output"].toString());
    check(labels.size() == 3 && labels.contains(a->streamId()) && labels.contains(b->streamId())
          && labels.contains(c->streamId()) && !labels.contains("DP-2"), "streams have independent control addresses");
    check(initial["streams"].toArray()[1].toObject()["paused"] == "1", "hidden activity starts paused");
    check(waitFor([&] { return firstWindow.grabWindow().pixelColor(16,16) == QColor(Qt::green); }),
          "visible consumer displays actual pixels");
    auto pauseIs = [&](Item *item, bool paused) {
        return waitFor([&] {
            auto command = lastControl(runtime, item->streamId(), "pause");
            return command.contains("pause") && command["pause"] == paused;
        });
    };
    firstActivity.setVisible(false);
    secondActivity.setVisible(true);
    check(pauseIs(a.get(), true) && pauseIs(b.get(), false), "parent activity switch pauses only its own stream");
    check(spawns(runtime).size() == 1, "activity visibility does not restart renderer");
    check(waitFor([&] { return firstWindow.grabWindow().pixelColor(16,16) == QColor(Qt::green); }),
          "newly visible consumer displays retained frame");
    b->setPaused(true);
    check(pauseIs(b.get(), true), "manual pause reaches active stream");
    secondActivity.setVisible(false);
    secondActivity.setVisible(true);
    check(pauseIs(b.get(), true) && b->paused(), "manual pause survives activity hide and show");
    secondActivity.setVisible(false);
    b->setPaused(false);
    check(pauseIs(b.get(), true) && !b->paused(), "unpausing hidden activity preserves effective pause");
    secondActivity.setVisible(true);
    check(pauseIs(b.get(), false), "show restores underlying unpaused state");
    firstWindow.hide();
    check(pauseIs(b.get(), true), "hiding window pauses visible item");
    firstWindow.show();
    check(pauseIs(b.get(), false), "showing window resumes visible item");
    const auto hiddenId = a->streamId();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(8,8), QPointF(8,8), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&firstWindow, &press);
    check(waitFor([&] { return lastControl(runtime,b->streamId(),"pointer")["pointer"].toObject()["buttons"] == 1; }),
          "pooled visible pointer press reaches presenter");
    check(lastControl(runtime,hiddenId,"pointer").isEmpty(), "hidden duplicate cannot inject pointer input");
    secondActivity.setVisible(false);
    firstActivity.setVisible(true);
    check(waitFor([&] { return lastControl(runtime,b->streamId(),"pointer")["pointer"].toObject()["buttons"] == 0; }),
          "activity switch releases held pointer buttons");
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(8,8), QPointF(8,8), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&firstWindow, &release);
    check(waitFor([&] { return lastControl(runtime,a->streamId(),"pointer")["pointer"].toObject()["buttons"] == 0; }),
          "newly active consumer accepts release input");
    QCoreApplication::sendEvent(&firstWindow, &press);
    check(waitFor([&] { return lastControl(runtime,a->streamId(),"pointer")["pointer"].toObject()["buttons"] == 1; }),
          "active consumer receives press before manual pause");
    a->setPaused(true);
    QCoreApplication::sendEvent(&firstWindow, &release);
    check(waitFor([&] { return lastControl(runtime,a->streamId(),"pointer")["pointer"].toObject()["buttons"] == 0; }),
          "manual pause releases held pointer buttons");
    a->setPaused(false);
    check(pauseIs(a.get(),false), "manual resume follows released pointer state");
    const QString cId = c->streamId();
    const auto beforeRename = spawns(runtime).size();
    c->setOutput("DP-2");
    check(waitFor([&] { return spawns(runtime).size() > beforeRename && ready(); }), "monitor rename restarts valid shared composition");
    check(c->streamId() == cId && SkwdWorkerPool::instance()->workerCount() == 1
          && spawns(runtime).last().toObject()["assignment"].toObject()["outputs"].toArray() == QJsonArray{"DP-2"},
          "monitor rename preserves token and permits three instances of same monitor");
    const auto beforeEmpty = spawns(runtime).size();
    c->setOutput("");
    check(waitFor([&] { return !c->sharesWorker() && spawns(runtime).size() > beforeEmpty; }),
          "missing monitor detaches consumer from shared group");
    c->setOutput("DP-3");
    check(waitFor(ready) && c->streamId() == cId, "monitor restoration rejoins with stable token");
    b->setAssignment(assignment("DP-2", "/wall/blue.png"));
    check(waitFor([&] { return SkwdWorkerPool::instance()->workerCount() == 2 && ready(); }),
          "different wallpaper splits only that consumer");
    b->setAssignment(assignment("DP-2"));
    check(waitFor([&] { return SkwdWorkerPool::instance()->workerCount() == 1 && ready(); }),
          "matching wallpaper rejoins original renderer");
    b->setAssignment(assignment("DP-2", "/wall/green.png", 35));
    check(waitFor([&] { return SkwdWorkerPool::instance()->workerCount() == 2 && ready(); }),
          "incompatible settings split duplicate consumer");
    b->setAssignment(assignment("DP-2"));
    check(waitFor([&] { return SkwdWorkerPool::instance()->workerCount() == 1 && ready(); }),
          "matching settings merge duplicate consumers");
    b.reset();
    check(waitFor([&] { return a->workerReady() && c->workerReady(); }), "destroying hidden consumer leaves other streams working");
    b = make(&secondActivity,"DP-2");
    check(waitFor(ready) && b->streamId() != originalBId && b->streamId() != cId,
          "recreated consumer has fresh identity and rejoins");
    const auto beforeTransient = spawns(runtime).size();
    for (int index = 0; index < 100; ++index) {
        auto transient = make(&secondActivity, "DP-2", "/wall/transient.png");
    }
    auto survivor = make(&secondActivity, "DP-2", "/wall/transient.png");
    check(waitFor([&] { return survivor->workerReady(); }), "consumer deletion before spawn does not corrupt next worker");
    check(spawns(runtime).size() == beforeTransient + 1, "stale spawn callbacks cannot launch replacement worker twice");
    auto differentDriver = make(&secondActivity, "DP-2");
    differentDriver->setSharedImageDevice("device", "driver-one");
    differentDriver->scheduleRestart();
    auto otherDriver = make(&secondActivity, "DP-2");
    otherDriver->setSharedImageDevice("device", "driver-two");
    otherDriver->scheduleRestart();
    check(waitFor([&] { return differentDriver->workerReady() && otherDriver->workerReady()
        && SkwdWorkerPool::instance()->workerCount() == 4; }), "different GPU driver identities cannot share imported memory");
    std::vector<std::unique_ptr<Item>> many;
    for (int index = 0; index < 20; ++index) many.push_back(make(&secondActivity,"DP-2","/wall/many.png"));
    check(waitFor([&] { return std::all_of(many.begin(), many.end(), [](const auto &item) { return item->workerReady(); }); }),
          "twenty static consumers exceed old descriptor cap without splitting renderer");
    auto largest = spawns(runtime).last().toObject();
    check(largest["streams"].toArray().size() == 20
          && largest["assignment"].toObject()["outputs"].toArray() == QJsonArray{"DP-2"}
          && SkwdWorkerPool::instance()->workerCount() == 5, "twenty streams use one renderer and one physical output");
    auto descriptorCount = [] { return QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(); };
    const auto beforeResourceTest = descriptorCount();
    struct rlimit originalLimit {};
    check(::getrlimit(RLIMIT_NOFILE, &originalLimit) == 0, "read descriptor resource limit");
    struct rlimit limited = originalLimit;
    limited.rlim_cur = qMin(rlim_t(beforeResourceTest + 24), originalLimit.rlim_cur);
    check(::setrlimit(RLIMIT_NOFILE, &limited) == 0, "constrain descriptor allocation for failure test");
    const auto previousHandler = qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &message) {
        resourceErrors.append(message);
    });
    std::vector<std::unique_ptr<Item>> pressured;
    for (int index = 0; index < 20; ++index) pressured.push_back(make(&secondActivity,"DP-2","/wall/pressure.png"));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const bool restored = ::setrlimit(RLIMIT_NOFILE, &originalLimit) == 0;
    const bool failed = waitFor([&] {
        return std::any_of(resourceErrors.begin(), resourceErrors.end(), [](const auto &line) {
            return line.contains("Cannot create the wallpaper") || line.contains("Cannot start")
                || line.contains("Wallpaper renderer exited");
        });
    });
    qInstallMessageHandler(previousHandler);
    check(restored && failed, "descriptor exhaustion reports failure without hanging");
    for (auto &item : pressured) item->scheduleRestart();
    check(waitFor([&] { return std::all_of(pressured.begin(), pressured.end(), [](const auto &item) { return item->workerReady(); }); }),
          "restoring descriptor limit recovers every consumer in shared renderer");
    pressured.clear();
    check(waitFor([&] { return descriptorCount() <= beforeResourceTest + 2; }), "failed stream setup does not leak descriptors");
    const QString artifacts = qEnvironmentVariable("SKWD_TEST_ARTIFACT_DIR");
    if (!artifacts.isEmpty()) {
        QDir().mkpath(artifacts);
        QFile::copy(runtime + "/events.jsonl", artifacts + "/activity-events.jsonl");
        firstWindow.grabWindow().save(artifacts + "/activity.png");
    }
}

int main(int argc, char **argv)
{
    QTemporaryDir runtime;
    qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
    QQuickWindow::setSceneGraphBackend("software");
    QGuiApplication app(argc, argv);
    try {
        run(runtime.path());
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n' << QJsonDocument(events(runtime.path())).toJson().toStdString();
        return 1;
    }
}
