#include "application/PipelineWorker.h"
#include "domain/Project.h"
#include "infrastructure/RecordRepository.h"
#include "infrastructure/SimulationPlant.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QSemaphore>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <thread>

using namespace ivs;

namespace {
ProjectConfig project() {
    auto p = defaultProject();
    p.resolution = QSize(640, 640);
    p.cycleMs = 100;
    p.imagePolicy = QStringLiteral("none");
    return p;
}

qint64 rowCount(const ProjectConfig &p, const QString &workspace) {
    RecordRepository repository;
    QString error;
    if (!repository.open(p.database, workspace, &error))
        return -1;
    return repository.count({}, &error);
}

bool containsState(const QSignalSpy &spy, const QString &state) {
    for (const auto &event : spy)
        if (event.first().toString() == state)
            return true;
    return false;
}

class TriggerServer {
  public:
    explicit TriggerServer(bool alwaysWaiting = false)
        : worker([this, alwaysWaiting] {
              QTcpServer server;
              if (server.listen(QHostAddress::LocalHost, 0))
                  port = server.serverPort();
              else
                  error = server.errorString();
              ready.release();
              if (!port || !server.waitForNewConnection(1500))
                  return;
              auto *socket = server.nextPendingConnection();
              while (true) {
                  QByteArray request;
                  while (request.size() < 12) {
                      if (!socket->bytesAvailable() && !socket->waitForReadyRead(1000))
                          return;
                      request += socket->readAll();
                  }
                  QByteArray reply;
                  if (request.at(7) == char(3)) {
                      ++reads;
                      reply = request.left(4) + QByteArray::fromHex("0005") + request.mid(6, 1) +
                              QByteArray::fromHex(alwaysWaiting || reads == 1 ? "03020000" : "03020001");
                  } else {
                      lastWrite = request.mid(7);
                      reply = request;
                  }
                  socket->write(reply);
                  socket->waitForBytesWritten(1000);
                  if (!lastWrite.isEmpty())
                      return;
              }
          }) {
        ready.acquire();
    }
    ~TriggerServer() {
        join();
    }
    void join() {
        if (worker.joinable())
            worker.join();
    }
    quint16 port = 0;
    QString error;
    int reads = 0;
    QByteArray lastWrite;

  private:
    QSemaphore ready;
    std::thread worker;
};
} // namespace

class PipelineTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        qRegisterMetaType<ivs::ProjectConfig>();
        qRegisterMetaType<ivs::InspectionResult>();
    }

    void singleCycleCommitsRealImageMeasurement() {
        QTemporaryDir workspace;
        QVERIFY(workspace.isValid());
        const auto p = project();
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(cycles.size(), 1, 2000);
        QCOMPARE(results.size(), 1);
        const auto result = qvariant_cast<InspectionResult>(results.first().first());
        QVERIFY(result.record.id > 0);
        QCOMPARE(result.record.outcome, QStringLiteral("OK"));
        QCOMPARE(result.record.diameterMm, 20.0);
        QCOMPARE(result.diameterPixels, 400.0);
        QCOMPARE(result.objectBounds.width(), 400.0);
        QVERIFY(!result.frame.isNull());
        QCOMPARE(rowCount(p, workspace.path()), 1);
        QVERIFY(containsState(states, QStringLiteral("running")));
        QVERIFY(containsState(states, QStringLiteral("stopped")));
    }

    void simulatedLowLightFaultsBeforeCommit() {
        QTemporaryDir workspace;
        auto p = project();
        for (auto &node : p.flow)
            if (node.type == "light")
                node.parameters = {{"brightness", 0}};
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void simulatedMoveWaitsBeforeCapture() {
        QTemporaryDir workspace;
        auto p = project();
        for (auto &node : p.flow)
            if (node.type == "move")
                node.parameters = {{"position", 2}, {"speed", 10}, {"timeoutMs", 1000}};
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QElapsedTimer elapsed;
        elapsed.start();
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 2000);
        QVERIFY(elapsed.elapsed() >= 180);
        const auto result = qvariant_cast<InspectionResult>(results.first().first());
        QVERIFY(result.record.offsetXmm > 1.0);
    }

    void sharedPlantAllowsManualTriggersAndObservesOutput() {
        QTemporaryDir workspace;
        const auto p = project();
        auto plant = std::make_shared<SimulationPlant>();
        QString error;
        QVERIFY(plant->configure(p.devices, &error));
        plant->setAutomaticTrigger(false);
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        worker.start(p, workspace.path(), true);
        QVERIFY(!worker.setSimulationPlant(std::make_shared<SimulationPlant>()));
        QTest::qWait(100);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
        QVERIFY(plant->queueTrigger("plc-1", &error));
        QTRY_COMPARE_WITH_TIMEOUT(cycles.size(), 1, 2000);
        QCOMPARE(results.size(), 1);
        quint16 output = 0;
        QVERIFY(plant->readSignal("plc-1", "holding", 0, &output, &error));
        QCOMPARE(output, quint16(1));
        const auto state = plant->snapshot();
        for (const auto &device : state.devices)
            if (device.id == "plc-1")
                QCOMPARE(device.pendingTriggers, 0);
    }

    void defaultCoilOutputRepresentsAcceptance_data() {
        QTest::addColumn<QString>("partMode");
        QTest::addColumn<quint16>("expectedBit");
        QTest::newRow("accepted") << QStringLiteral("good") << quint16(1);
        QTest::newRow("rejected") << QStringLiteral("ng") << quint16(0);
    }

    void defaultCoilOutputRepresentsAcceptance() {
        QFETCH(QString, partMode);
        QFETCH(quint16, expectedBit);
        QTemporaryDir workspace;
        auto p = project();
        for (auto &node : p.flow)
            if (node.type == "output")
                node.parameters = {{"deviceId", "io-1"}, {"function", "coil"}, {"address", 1}};
        auto plant = std::make_shared<SimulationPlant>();
        QString error;
        QVERIFY(plant->configure(p.devices, &error));
        QVERIFY(plant->setPartMode(partMode, &error));
        QVERIFY(plant->writeSignal("io-1", "coil", 1, expectedBit ? 0 : 1, &error));
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(cycles.size(), 1, 2000);
        quint16 value = 2;
        QVERIFY(plant->readSignal("io-1", "coil", 1, &value, &error));
        QCOMPARE(value, expectedBit);
        QCOMPARE(rowCount(p, workspace.path()), 1);
    }

    void cancellationStopsVirtualMotion_data() {
        QTest::addColumn<QString>("action");
        QTest::newRow("stop") << QStringLiteral("stop");
        QTest::newRow("shutdown") << QStringLiteral("shutdown");
        QTest::newRow("emergency") << QStringLiteral("emergency");
        QTest::newRow("disconnect") << QStringLiteral("disconnect");
        QTest::newRow("manual-stop") << QStringLiteral("manual-stop");
        QTest::newRow("manual-retarget") << QStringLiteral("manual-retarget");
    }

    void cancellationStopsVirtualMotion() {
        QFETCH(QString, action);
        QTemporaryDir workspace;
        auto p = project();
        for (auto &node : p.flow)
            if (node.type == "move")
                node.parameters = {{"position", 5}, {"speed", 2}, {"timeoutMs", 5000}};
        auto plant = std::make_shared<SimulationPlant>();
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        worker.start(p, workspace.path(), true);
        auto position = [&] { return plant->snapshot().devices.at(1).axes.at(0).position; };
        QTRY_VERIFY_WITH_TIMEOUT(position() > 0.05, 1000);
        if (action == "stop")
            worker.stop();
        else if (action == "shutdown")
            worker.shutdown();
        else {
            if (action == "emergency")
                plant->setEmergencyStop(true);
            else {
                QString error;
                if (action == "manual-stop")
                    QVERIFY(plant->stopAxis("motion-1", 0, &error));
                else if (action == "manual-retarget")
                    QVERIFY(plant->moveAxis("motion-1", 0, 1, 2, &error));
                else
                    QVERIFY(plant->setConnected("motion-1", false, &error));
            }
            QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        }
        const double stoppedAt = position();
        QVERIFY(stoppedAt < 5);
        QTest::qWait(150);
        QCOMPARE(position(), stoppedAt);
        QVERIFY(!plant->snapshot().devices.at(1).axes.at(0).moving);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void transientVirtualInterruptionRequiresExplicitRestart_data() {
        QTest::addColumn<QString>("interruption");
        QTest::newRow("emergency-cleared-during-delay") << QStringLiteral("emergency");
        QTest::newRow("disconnect-reconnected-during-delay") << QStringLiteral("disconnect");
    }

    void transientVirtualInterruptionRequiresExplicitRestart() {
        QFETCH(QString, interruption);
        QTemporaryDir workspace;
        auto p = project();
        p.flow.insert(3, {"delay", "delay", "Wait", true, {{"ms", 5000}}});
        auto plant = std::make_shared<SimulationPlant>();
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy nodes(&worker, &PipelineWorker::nodeActivated);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto &event : nodes)
                    if (event.first().toString() == "delay")
                        return true;
                return false;
            }(),
            1000);
        QString error;
        if (interruption == "emergency")
            plant->setEmergencyStop(true);
        else
            QVERIFY(plant->setConnected("camera-1", false, &error));
        QTest::qWait(20);
        if (interruption == "emergency")
            plant->setEmergencyStop(false);
        else
            QVERIFY(plant->setConnected("camera-1", true, &error));
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 6000);
        QCOMPARE(results.size(), 0);
        QCOMPARE(cycles.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(cycles.size(), 1, 6000);
        QCOMPARE(results.size(), 1);
        QCOMPARE(rowCount(p, workspace.path()), 1);
    }

    void startupRejectsActiveVirtualFaultBeforeItCanBeCleared_data() {
        QTest::addColumn<QString>("interruption");
        QTest::newRow("emergency") << QStringLiteral("emergency");
        QTest::newRow("disconnect") << QStringLiteral("disconnect");
    }

    void startupRejectsActiveVirtualFaultBeforeItCanBeCleared() {
        QFETCH(QString, interruption);
        QTemporaryDir workspace;
        const auto p = project();
        auto plant = std::make_shared<SimulationPlant>();
        QString error;
        QVERIFY(plant->configure(p.devices, &error));
        if (interruption == "emergency")
            plant->setEmergencyStop(true);
        else
            QVERIFY(plant->setConnected("camera-1", false, &error));
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        if (interruption == "emergency")
            plant->setEmergencyStop(false);
        else
            QVERIFY(plant->setConnected("camera-1", true, &error));
        QVERIFY(containsState(states, QStringLiteral("fault")));
        QVERIFY(!containsState(states, QStringLiteral("running")));
        QTest::qWait(50);
        QCOMPARE(results.size(), 0);
        QVERIFY(!QFileInfo::exists(QDir(workspace.path()).filePath(p.database.database)));
    }

    void robotSortUsesCommittedOutcomeAndCompletesRealVirtualRelease_data() {
        QTest::addColumn<QString>("partMode");
        QTest::addColumn<qint64>("okCount");
        QTest::addColumn<qint64>("ngCount");
        QTest::newRow("accepted") << QStringLiteral("good") << qint64(1) << qint64(0);
        QTest::newRow("rejected") << QStringLiteral("ng") << qint64(0) << qint64(1);
    }

    void robotSortUsesCommittedOutcomeAndCompletesRealVirtualRelease() {
        QFETCH(QString, partMode);
        QFETCH(qint64, okCount);
        QFETCH(qint64, ngCount);
        QTemporaryDir workspace;
        auto p = project();
        p.devices.append({"scara-test", "robot", "SCARA", "simulated"});
        p.flow.append({"robot-sort",
                       "robot_sort",
                       "视觉分拣",
                       true,
                       {{"deviceId", "scara-test"}, {"restock", true}, {"timeoutMs", 10000}}});
        qint64 now = 0;
        auto plant = std::make_shared<SimulationPlant>([&] { return now; });
        QVERIFY(plant->setPartMode(partMode));
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        QTimer clock;
        connect(&clock, &QTimer::timeout, &worker, [&] { now += 1000; });
        clock.start(10);
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(cycles.size(), 1, 3000);
        QCOMPARE(results.size(), 1);
        QCOMPARE(rowCount(p, workspace.path()), 1);
        const auto result = qvariant_cast<InspectionResult>(results.first().first());
        QCOMPARE(result.record.outcome, okCount ? QStringLiteral("OK") : QStringLiteral("NG"));
        const auto robot = plant->snapshot().devices.last().robot;
        QCOMPARE(robot.okCount, okCount);
        QCOMPARE(robot.ngCount, ngCount);
        QVERIFY(!robot.carryingPart);
        QVERIFY(!robot.programRunning);
        QCOMPARE(robot.completedSequence, robot.commandSequence);
    }

    void robotMoveWaitsForSharedStateAndGripsOnce() {
        QTemporaryDir workspace;
        auto p = project();
        p.flow.insert(
            3,
            {"robot-move", "robot_move", "机械臂定位", true, {{"x", 220}, {"y", 40}, {"z", 20}, {"r", 0}}});
        p.flow.insert(4, {"robot-grip", "robot_grip", "机械臂夹爪", true, {{"closed", true}}});
        qint64 now = 0;
        auto plant = std::make_shared<SimulationPlant>([&] { return now; });
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(plant->snapshot().devices.last().robot.moving, 1000);
        const quint64 moveSequence = plant->snapshot().devices.last().robot.commandSequence;
        QTest::qWait(50);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
        now = 100000;
        QTRY_COMPARE_WITH_TIMEOUT(cycles.size(), 1, 1000);
        const auto robot = plant->snapshot().devices.last().robot;
        QVERIFY(robot.carryingPart);
        QCOMPARE(robot.commandSequence, moveSequence + 1);
        QCOMPARE(results.size(), 1);
    }

    void robotSortCancellationPreservesCommittedRecordWithoutFinishingCycle_data() {
        QTest::addColumn<QString>("action");
        for (const auto &action :
             {"stop", "shutdown", "console-stop", "retarget", "disable", "emergency", "disconnect"})
            QTest::newRow(action) << QString::fromLatin1(action);
    }

    void robotSortCancellationPreservesCommittedRecordWithoutFinishingCycle() {
        QFETCH(QString, action);
        QTemporaryDir workspace;
        auto p = project();
        p.flow.append({"robot-sort", "robot_sort", "视觉分拣", true, {{"restock", true}}});
        qint64 now = 0;
        auto plant = std::make_shared<SimulationPlant>([&] { return now; });
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(plant->snapshot().devices.last().robot.programRunning, 1000);
        QCOMPARE(results.size(), 1);
        QString error;
        if (action == "stop")
            worker.stop();
        else if (action == "shutdown")
            worker.shutdown();
        else {
            if (action == "console-stop")
                QVERIFY(plant->stopRobot("robot-1", &error));
            else if (action == "retarget") {
                // Programs reject direct manual retargeting; a stopped program may receive a new command.
                QVERIFY(plant->stopRobot("robot-1", &error));
                QVERIFY(plant->moveRobotTcp("robot-1", {180, 140, 100, 0}, 60, true, &error));
            } else if (action == "disable")
                QVERIFY(plant->enableRobot("robot-1", false, &error));
            else if (action == "emergency") {
                plant->setEmergencyStop(true);
                plant->setEmergencyStop(false);
            } else {
                QVERIFY(plant->setConnected("robot-1", false, &error));
                QVERIFY(plant->setConnected("robot-1", true, &error));
            }
            QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        }
        const auto stopped = plant->snapshot().devices.last().robot;
        now = 100000;
        QTest::qWait(50);
        const auto after = plant->snapshot().devices.last().robot;
        QCOMPARE(after.joints.j1, stopped.joints.j1);
        QCOMPARE(after.joints.j2, stopped.joints.j2);
        QCOMPARE(after.joints.z, stopped.joints.z);
        QCOMPARE(after.joints.j4, stopped.joints.j4);
        QVERIFY(!after.moving);
        QVERIFY(!after.programRunning);
        QCOMPARE(after.okCount, qint64(0));
        QCOMPARE(after.ngCount, qint64(0));
        QCOMPARE(results.size(), 1);
        QCOMPARE(cycles.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 1);
    }

    void robotMoveTimeoutCannotStoreOrComplete() {
        QTemporaryDir workspace;
        auto p = project();
        p.flow.insert(3, {"robot-move",
                          "robot_move",
                          "机械臂定位",
                          true,
                          {{"x", 220}, {"y", 40}, {"z", 20}, {"timeoutMs", 40}}});
        auto plant = std::make_shared<SimulationPlant>([] { return qint64(0); });
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        QCOMPARE(results.size(), 0);
        QCOMPARE(cycles.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
        const auto robot = plant->snapshot().devices.last().robot;
        QVERIFY(!robot.moving);
        QCOMPARE(robot.interruptedSequence, robot.commandSequence);
    }

    void invalidRobotConfigurationFaultsBeforeOpeningDatabase_data() {
        QTest::addColumn<QString>("invalid");
        QTest::newRow("pose") << QStringLiteral("pose");
        QTest::newRow("route") << QStringLiteral("route");
        QTest::newRow("backend") << QStringLiteral("backend");
    }

    void invalidRobotConfigurationFaultsBeforeOpeningDatabase() {
        QFETCH(QString, invalid);
        QTemporaryDir workspace;
        auto p = project();
        FlowNode node{"robot-move", "robot_move", "机械臂定位", true, {}};
        if (invalid == "pose")
            node.parameters = {{"x", 500}};
        else if (invalid == "route")
            node.parameters = {{"deviceId", "motion-1"}};
        else
            p.devices.last().backend = "modbus-tcp";
        p.flow.insert(3, node);
        PipelineWorker worker;
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QVERIFY(containsState(states, QStringLiteral("fault")));
        QCOMPARE(results.size(), 0);
        QVERIFY(!QFileInfo::exists(QDir(workspace.path()).filePath(p.database.database)));
    }

    void motionTimeoutStopsAxesBeforeStore() {
        QTemporaryDir workspace;
        auto p = project();
        for (auto &node : p.flow)
            if (node.type == "move")
                node.parameters = {{"position", 5}, {"speed", 1}, {"timeoutMs", 60}};
        auto plant = std::make_shared<SimulationPlant>();
        PipelineWorker worker;
        QVERIFY(worker.setSimulationPlant(plant));
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        QVERIFY(!plant->snapshot().devices.at(1).axes.at(0).moving);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void fifthPartIsMeasuredAndJudgedNg() {
        QTemporaryDir workspace;
        const auto p = project();
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        connect(&worker, &PipelineWorker::cycleFinished, &worker, [&] {
            if (results.size() == 5)
                worker.stop();
        });
        worker.start(p, workspace.path());
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 5, 3000);
        const auto fifth = qvariant_cast<InspectionResult>(results.at(4).first());
        QCOMPARE(fifth.record.outcome, QStringLiteral("NG"));
        QCOMPARE(fifth.record.diameterMm, 23.0);
        QCOMPARE(fifth.diameterPixels, 460.0);
        QTest::qWait(150);
        QCOMPARE(results.size(), 5);
        QCOMPARE(rowCount(p, workspace.path()), 5);
    }

    void invalidOrderFaultsBeforeOpeningDatabase() {
        QTemporaryDir workspace;
        auto p = project();
        p.flow.swapItemsAt(3, 6); // store now precedes capture/inspect/judge.
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy logs(&worker, &PipelineWorker::logMessage);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 500);
        QCOMPARE(results.size(), 0);
        QVERIFY(!logs.isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(workspace.path()).filePath(p.database.database)));
    }

    void stopDuringDelayPreventsStore() {
        QTemporaryDir workspace;
        auto p = project();
        p.flow.insert(3, {"delay", "delay", "Wait", true, {{"ms", 1000}}});
        PipelineWorker worker;
        QSignalSpy nodes(&worker, &PipelineWorker::nodeActivated);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path());
        QTRY_VERIFY_WITH_TIMEOUT(
            [&] {
                for (const auto &event : nodes)
                    if (event.first().toString() == "delay")
                        return true;
                return false;
            }(),
            1000);
        worker.stop();
        QTest::qWait(1150);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void databasePathFailureEmitsFault() {
        QTemporaryDir workspace;
        QFile blocker(QDir(workspace.path()).filePath("blocked"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        auto p = project();
        p.database.database = QStringLiteral("blocked/inspection.sqlite");
        PipelineWorker worker;
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy logs(&worker, &PipelineWorker::logMessage);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 500);
        QVERIFY(!logs.isEmpty());
        QCOMPARE(results.size(), 0);
    }

    void imageSaveFailureNeverClaimsCommittedRecord() {
        QTemporaryDir workspace;
        QVERIFY(QDir(workspace.path()).mkpath("data"));
        QFile blocker(QDir(workspace.path()).filePath("data/images"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        auto p = project();
        p.imagePolicy = QStringLiteral("all");
        PipelineWorker worker;
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void databaseInsertFailureNeverEmitsSavedResult() {
        QTemporaryDir workspace;
        const auto p = project();
        QCOMPARE(rowCount(p, workspace.path()), 0); // Create the real schema first.
        const QString connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(QDir(workspace.path()).filePath(p.database.database));
            QVERIFY(database.open());
            QSqlQuery sql(database);
            QVERIFY(sql.exec("CREATE TRIGGER reject_inspection BEFORE INSERT ON inspection_records BEGIN "
                             "SELECT RAISE(ABORT, 'test storage unavailable'); END"));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        PipelineWorker worker;
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void retainedImagesStayInsideManagedDirectory() {
        QTemporaryDir workspace;
        auto p = project();
        p.imagePolicy = QStringLiteral("all");
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 2000);
        const auto result = qvariant_cast<InspectionResult>(results.first().first());
        QVERIFY(QDir::isRelativePath(result.record.imagePath));
        QVERIFY(result.record.imagePath.startsWith("data/images/ivs-managed/"));
        const QString imagePath = QDir(workspace.path()).filePath(result.record.imagePath);
        QVERIFY(QFileInfo::exists(imagePath));
        QCOMPARE(QImage(imagePath).size(), p.resolution);
    }

    void savedNgImageMovesWithEntireWorkspace() {
        QTemporaryDir container;
        QVERIFY(container.isValid());
        const QString original = QDir(container.path()).filePath("original");
        const QString relocated = QDir(container.path()).filePath("relocated");
        QVERIFY(QDir().mkpath(original));
        auto p = project();
        p.imagePolicy = QStringLiteral("ng");
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        connect(&worker, &PipelineWorker::cycleFinished, &worker, [&] {
            if (results.size() == 5)
                worker.stop();
        });
        worker.start(p, original);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 5, 3000);
        worker.stop(); // Close SQLite before moving its directory, including on Windows.
        const auto saved = qvariant_cast<InspectionResult>(results.last().first());
        QCOMPARE(saved.record.outcome, QStringLiteral("NG"));
        QVERIFY(QDir().rename(original, relocated));
        RecordRepository repository;
        QString error;
        QVERIFY2(repository.open(p.database, relocated, &error), qPrintable(error));
        RecordFilter filter;
        filter.outcome = QStringLiteral("NG");
        const auto records = repository.query(filter, &error);
        QCOMPARE(records.size(), 1);
        const QString storedPath = records.first().imagePath;
        QVERIFY2(QDir::isRelativePath(storedPath), qPrintable(storedPath));
        QVERIFY(!storedPath.contains(original));
        QVERIFY(QFileInfo::exists(QDir(relocated).filePath(storedPath)));
        QCOMPARE(QImage(QDir(relocated).filePath(storedPath)).size(), p.resolution);
        QVERIFY(!QFileInfo::exists(original));
    }

    void quotaPrunesOnlyManagedImages() {
        QTemporaryDir workspace;
        auto p = project();
        p.imagePolicy = QStringLiteral("all");
        p.imageQuotaMB = 1;
        const auto relative = QStringLiteral("data/images/ivs-managed");
        QVERIFY(QDir(workspace.path()).mkpath(relative));
        const QString directory = QDir(workspace.path()).filePath(relative);
        const QString oldImage = QDir(directory).filePath("ivs-00000000000000000000000000000000.png");
        const QString unrelated = QDir(directory).filePath("keep-customer-file.png");
        QFile old(oldImage);
        QVERIFY(old.open(QIODevice::WriteOnly));
        QCOMPARE(old.write(QByteArray(2 * 1024 * 1024, 'x')), 2 * 1024 * 1024);
        old.close();
        QFile customer(unrelated);
        QVERIFY(customer.open(QIODevice::WriteOnly));
        QCOMPARE(customer.write("customer"), 8);
        customer.close();
        PipelineWorker worker;
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 2000);
        QVERIFY(!QFileInfo::exists(oldImage));
        QVERIFY(QFileInfo::exists(unrelated));
        QVERIFY(QFileInfo::exists(
            QDir(workspace.path())
                .filePath(qvariant_cast<InspectionResult>(results.first().first()).record.imagePath)));
    }

    void captureRejectsDeviceWithWrongCategory() {
        QTemporaryDir workspace;
        auto p = project();
        for (auto &node : p.flow)
            if (node.type == "capture")
                node.parameters.insert("deviceId", "motion-1");
        PipelineWorker worker;
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void triggerPollsAndOutputEncodesCommittedOutcome() {
        TriggerServer peer;
        QVERIFY2(peer.port, qPrintable(peer.error));
        QTemporaryDir workspace;
        auto p = project();
        for (auto &device : p.devices)
            if (device.id == "plc-1") {
                device.backend = "modbus-tcp";
                device.port = peer.port;
            }
        for (auto &node : p.flow) {
            if (node.type == "trigger")
                node.parameters = {{"requireValue", 1}, {"pollMs", 10}, {"timeoutMs", 1000}};
            if (node.type == "output")
                node.parameters = {{"address", 20}};
        }
        PipelineWorker worker;
        QSignalSpy cycles(&worker, &PipelineWorker::cycleFinished);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_COMPARE_WITH_TIMEOUT(cycles.size(), 1, 2000);
        peer.join();
        QCOMPARE(peer.reads, 2);
        QCOMPARE(peer.lastWrite.toHex(), QByteArray("0600140001"));
        QCOMPARE(results.size(), 1);
        QVERIFY(qvariant_cast<InspectionResult>(results.first().first()).record.id > 0);
    }

    void triggerTimeoutFaultsBeforeStore() {
        TriggerServer peer(true);
        QVERIFY2(peer.port, qPrintable(peer.error));
        QTemporaryDir workspace;
        auto p = project();
        for (auto &device : p.devices)
            if (device.id == "plc-1") {
                device.backend = "modbus-tcp";
                device.port = peer.port;
            }
        for (auto &node : p.flow)
            if (node.type == "trigger")
                node.parameters = {{"requireValue", 1}, {"pollMs", 10}, {"timeoutMs", 60}};
        PipelineWorker worker;
        QSignalSpy states(&worker, &PipelineWorker::stateChanged);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path(), true);
        QTRY_VERIFY_WITH_TIMEOUT(containsState(states, QStringLiteral("fault")), 1000);
        peer.join();
        QVERIFY(peer.reads >= 2);
        QVERIFY(peer.lastWrite.isEmpty());
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void shutdownCancelsPendingTimers() {
        QTemporaryDir workspace;
        auto p = project();
        p.flow.insert(3, {"delay", "delay", "Wait", true, {{"ms", 1000}}});
        PipelineWorker worker;
        QSignalSpy nodes(&worker, &PipelineWorker::nodeActivated);
        QSignalSpy results(&worker, &PipelineWorker::resultReady);
        worker.start(p, workspace.path());
        QTRY_VERIFY_WITH_TIMEOUT(nodes.size() >= 4, 1000);
        worker.shutdown();
        QTest::qWait(1100);
        QCOMPARE(results.size(), 0);
        QCOMPARE(rowCount(p, workspace.path()), 0);
    }

    void workerAndResourcesRunOnMovedThread() {
        QTemporaryDir workspace;
        QThread thread;
        auto *worker = new PipelineWorker;
        auto plant = std::make_shared<SimulationPlant>();
        plant->setAutomaticTrigger(false);
        QVERIFY(worker->setSimulationPlant(plant));
        worker->moveToThread(&thread);
        connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        QSignalSpy results(worker, &PipelineWorker::resultReady);
        thread.start();
        const auto p = project();
        QVERIFY(QMetaObject::invokeMethod(
            worker, [worker, p, path = workspace.path()] { worker->start(p, path, true); },
            Qt::QueuedConnection));
        QTest::qWait(100);
        const bool waitingForTrigger = results.isEmpty();
        QString triggerError;
        const bool queued = plant->queueTrigger("plc-1", &triggerError);
        const bool rejectedCrossThreadInjection =
            !worker->setSimulationPlant(std::make_shared<SimulationPlant>());
        const bool received = results.wait(2000);
        const bool shutDown =
            QMetaObject::invokeMethod(worker, &PipelineWorker::shutdown, Qt::BlockingQueuedConnection);
        thread.quit();
        QVERIFY(thread.wait(2000));
        QVERIFY(waitingForTrigger);
        QVERIFY2(queued, qPrintable(triggerError));
        QVERIFY(rejectedCrossThreadInjection);
        QVERIFY(received);
        QVERIFY(shutDown);
        QCOMPARE(rowCount(p, workspace.path()), 1);
    }
};

QTEST_GUILESS_MAIN(PipelineTests)
#include "tst_pipeline.moc"
