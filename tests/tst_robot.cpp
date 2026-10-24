#include "domain/RobotKinematics.h"
#include "infrastructure/SimulationPlant.h"
#include <QJsonArray>
#include <QtTest>
#include <limits>

using namespace ivs;
namespace {
DeviceConfig config(const QString &id, const QString &category = "robot",
                    const QString &backend = "simulated") {
    DeviceConfig d;
    d.id = id;
    d.category = category;
    d.backend = backend;
    d.model = "scara";
    return d;
}
SimRobotState robot(const SimulationPlant &plant, const QString &id = "r") {
    for (const auto &device : plant.snapshot().devices)
        if (device.id == id)
            return device.robot;
    return {};
}
bool near(double a, double b) {
    return std::abs(a - b) < 1e-7;
}
} // namespace
class RobotTests : public QObject {
    Q_OBJECT
  private slots:
    void acceptsOnlySimulatedRobotDevices() {
        SimulationPlant plant;
        QString error;
        QVERIFY2(plant.configure({config("r"), config("real", "robot", "vendor")}, &error),
                 qPrintable(error));
        const auto snapshot = plant.snapshot();
        QCOMPARE(snapshot.devices.size(), 1);
        QCOMPARE(snapshot.devices[0].id, QString("r"));
        QCOMPARE(snapshot.devices[0].category, QString("robot"));
        QVERIFY(near(snapshot.devices[0].robot.tcp.x, 180));
        QVERIFY(near(snapshot.devices[0].robot.tcp.y, 140));
        QCOMPARE(snapshot.devices[0].robot.partLocation, QString("feed"));
        QCOMPARE(snapshot.devices[0].robot.trail.size(), 1);
    }
    void forwardAndBothInverseElbows() {
        const auto zero = robotForward({0, 0, 0, 0});
        QVERIFY(near(zero.x, 320));
        QVERIFY(near(zero.y, 0));
        QCOMPARE(zero.z, 0.0);
        QCOMPARE(zero.r, 0.0);
        const auto pose = robotForward({90, -90, 50, 180});
        QVERIFY(near(pose.x, 140));
        QVERIFY(near(pose.y, 180));
        QVERIFY(near(std::abs(pose.r), 180));
        for (bool positive : {true, false}) {
            RobotJoints joints;
            QVERIFY(robotInverse({220, 40, 20, 30}, positive, &joints));
            QVERIFY(positive ? joints.j2 > 0 : joints.j2 < 0);
            const auto actual = robotForward(joints);
            QVERIFY(near(actual.x, 220));
            QVERIFY(near(actual.y, 40));
            QVERIFY(near(actual.z, 20));
            QVERIFY(near(actual.r, 30));
        }
        RobotJoints untouched{1, 2, 3, 4};
        for (const RobotPose pose :
             {RobotPose{321, 0, 20, 0}, RobotPose{0, 0, 20, 0}, RobotPose{220, 40, -1, 0},
              RobotPose{220, 40, 151, 0}, RobotPose{220, 40, 20, std::numeric_limits<double>::infinity()}}) {
            QVERIFY(!robotInverse(pose, true, &untouched));
            QCOMPARE(untouched.j1, 1.0);
            QCOMPARE(untouched.j4, 4.0);
        }
        QVERIFY(!robotInverse({220, 40, 20, 0}, true, nullptr));
        QVERIFY(validateRobotJoints({-170, 170, 150, 180}));
        for (const RobotJoints invalid :
             {RobotJoints{-171, 0, 20, 0}, RobotJoints{0, 171, 20, 0}, RobotJoints{0, 0, 20, 181},
              RobotJoints{0, 0, -1, 0}, RobotJoints{0, 0, 151, 0},
              RobotJoints{0, 0, 20, std::numeric_limits<double>::quiet_NaN()}})
            QVERIFY(!validateRobotJoints(invalid));
    }
    void inverseAcceptsJointLimitEndpoints() {
        for (const RobotJoints source :
             {RobotJoints{170, 20, 150, 180}, RobotJoints{-170, -20, 0, -180}, RobotJoints{10, 170, 0, 180},
              RobotJoints{-10, -170, 150, -180}, RobotJoints{20, 0, 50, 0}}) {
            RobotJoints actual;
            QString error;
            QVERIFY2(robotInverse(robotForward(source), source.j2 > 0, &actual, &error), qPrintable(error));
            QVERIFY(near(actual.j1, source.j1));
            QVERIFY(near(actual.j2, source.j2));
            QVERIFY(validateRobotJoints(actual));
        }
    }
    void inverseRemainsUsableAcrossJointWorkspace() {
        for (int shoulder = -170; shoulder <= 170; shoulder += 10)
            for (int elbow = -170; elbow <= 170; elbow += 10) {
                RobotJoints source{double(shoulder), double(elbow), 50, 0};
                RobotJoints actual;
                QString error;
                QVERIFY2(robotInverse(robotForward(source), elbow >= 0, &actual, &error),
                         qPrintable(QString("J1=%1 J2=%2: %3").arg(shoulder).arg(elbow).arg(error)));
                QVERIFY(std::abs(actual.j1 - shoulder) < 1e-5);
                QVERIFY(std::abs(actual.j2 - elbow) < 1e-5);
            }
    }
    void fractionalFullExtensionRoundTripsAndRejectsOutsideWorkspace() {
        const QVector<double> shoulders{-170,   -169.99, -169.999999, -90.001, -45.123456789, -0.0001, 0.0001,
                                        27.815, 89.999,  169.99,      170};
        for (double shoulder : shoulders)
            for (double wrist : {-180.0, 0.0, 180.0}) {
                const RobotJoints source{shoulder, 0, 50, wrist};
                RobotJoints actual;
                QString error;
                QVERIFY2(
                    robotInverse(robotForward(source), true, &actual, &error),
                    qPrintable(QString("J1=%1 J4=%2: %3").arg(shoulder, 0, 'g', 17).arg(wrist).arg(error)));
                QVERIFY(std::abs(actual.j1 - shoulder) < 1e-5);
                QVERIFY(std::abs(actual.j2) < 1e-5);
                QVERIFY(validateRobotJoints(actual));
            }
        for (int hundredths = -16999; hundredths <= 16999; ++hundredths) {
            const double shoulder = hundredths / 100.0;
            RobotJoints actual;
            QString error;
            QVERIFY2(robotInverse(robotForward({shoulder, 0, 50, 0}), true, &actual, &error),
                     qPrintable(QString("J1=%1: %2").arg(shoulder, 0, 'g', 17).arg(error)));
        }
        RobotJoints untouched{1, 2, 3, 4};
        QVERIFY(!robotInverse({320.000001, 0, 50, 0}, true, &untouched));
        QVERIFY(!robotInverse({39.999999, 0, 50, 0}, true, &untouched));
        QCOMPARE(untouched.j1, 1.0);
        QCOMPARE(untouched.j4, 4.0);
    }
    void teachingJsonIsStrictAndAtomic() {
        QVector<RobotWaypoint> points{{"pick", {0, 90, 20, -90}, true}, {"place", {40, 30, 100, -70}, false}};
        const auto document = robotTeachingToJson(points);
        QCOMPARE(document.value("schema").toInt(), 1);
        QVector<RobotWaypoint> parsed;
        QVERIFY(robotTeachingFromJson(document, &parsed));
        QCOMPARE(parsed.size(), 2);
        QCOMPARE(parsed[0].name, QString("pick"));
        QCOMPARE(parsed[0].joints.z, 20.0);
        QVERIFY(parsed[0].gripperClosed);
        const auto original = parsed;
        QList<QJsonObject> bad;
        auto doc = document;
        doc["schema"] = 2;
        bad << doc;
        doc = document;
        doc["extra"] = true;
        bad << doc;
        doc = document;
        doc["schema"] = "1";
        bad << doc;
        for (const QJsonObject entry :
             {QJsonObject{{"name", ""},
                          {"joints", QJsonObject{{"j1", 0}, {"j2", 90}, {"z", 20}, {"j4", -90}}},
                          {"gripper", false}},
              QJsonObject{{"name", "p"},
                          {"joints", QJsonObject{{"j1", "0"}, {"j2", 90}, {"z", 20}, {"j4", -90}}},
                          {"gripper", false}},
              QJsonObject{{"name", "p"},
                          {"joints", QJsonObject{{"j1", 0}, {"j2", 90}, {"z", 20}, {"j4", -90}}},
                          {"gripper", 1}},
              QJsonObject{{"name", "p"},
                          {"joints", QJsonObject{{"j1", 0}, {"j2", 90}, {"z", 200}, {"j4", -90}}},
                          {"gripper", false}}})
            bad << QJsonObject{{"schema", 1}, {"points", QJsonArray{entry}}};
        for (const auto &invalid : bad) {
            QVERIFY(!robotTeachingFromJson(invalid, &parsed));
            QCOMPARE(parsed.size(), original.size());
            QCOMPARE(parsed[0].name, original[0].name);
        }
        QVERIFY(!robotTeachingFromJson(document, nullptr));
        QJsonArray tooMany;
        for (int i = 0; i < 101; ++i)
            tooMany.append(document["points"].toArray()[0]);
        QVERIFY(!robotTeachingFromJson({{"schema", 1}, {"points", tooMany}}, &parsed));
    }
    void jointsInterpolateSynchronouslyAndRetargetInterruptsToken() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r"), config("other")}));
        QVERIFY(plant.moveRobotJoints("r", {90, 0, 25, 0}, 100));
        const auto token = robot(plant).commandSequence;
        QCOMPARE(token, quint64(1));
        now = 500;
        auto value = robot(plant);
        QVERIFY(near(value.joints.j1, 45));
        QVERIFY(near(value.joints.j2, 45));
        QVERIFY(near(value.joints.z, 62.5));
        QVERIFY(near(value.joints.j4, -45));
        QVERIFY(value.moving);
        QCOMPARE(value.completedSequence, quint64(0));
        QCOMPARE(robot(plant, "other").joints.j1, 0.0);
        QVERIFY(plant.moveRobotJoints("r", {0, 90, 100, -90}, 50));
        value = robot(plant);
        QCOMPARE(value.interruptedSequence, token);
        QCOMPARE(value.commandSequence, token + 1);
        now = 1000;
        value = robot(plant);
        QVERIFY(value.moving);
        QVERIFY(near(value.joints.j1, 22.5));
        now = 1500;
        value = robot(plant);
        QVERIFY(!value.moving);
        QCOMPARE(value.completedSequence, token + 1);
        QCOMPARE(value.joints.j1, 0.0);
        QCOMPARE(value.joints.z, 100.0);
        QVERIFY(plant.homeRobot("r"));
        QVERIFY(!robot(plant).moving);
        QCOMPARE(robot(plant).completedSequence, token + 2);
    }
    void invalidCommandsAreAtomicAndNeverReroute() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r"), config("axis", "motion")}));
        QVERIFY(!plant.moveRobotJoints("missing", {}, 60));
        QVERIFY(!plant.moveRobotJoints("axis", {}, 60));
        QVERIFY(!plant.moveRobotJoints("", {}, 60));
        QVERIFY(!plant.moveRobotJoints("r", {171, 0, 0, 0}, 60));
        for (double speed : {0.0, 100.1, std::numeric_limits<double>::quiet_NaN()})
            QVERIFY(!plant.moveRobotJoints("r", {}, speed));
        QVERIFY(!plant.moveRobotTcp("r", {1000, 0, 10, 0}, 60));
        QCOMPARE(robot(plant).commandSequence, quint64(0));
        QVERIFY(plant.enableRobot("r", false));
        QVERIFY(!plant.moveRobotJoints("r", {}, 60));
        QVERIFY(!plant.setRobotGripper("r", true));
        QVERIFY(plant.enableRobot("r", true));
        QVERIFY(plant.setConnected("r", false));
        QVERIFY(!plant.homeRobot("r"));
        QVERIFY(!plant.restockRobot("r"));
        QVERIFY(plant.setConnected("r", true));
        plant.setEmergencyStop(true);
        QVERIFY(!plant.moveRobotTcp("r", {220, 40, 20, 0}, 60));
        QVERIFY(!plant.setRobotGripper("r", true));
        plant.setEmergencyStop(false);
        QVERIFY(plant.moveRobotTcp("r", {220, 40, 20, 0}, 100, false));
        now = 100000;
        QVERIFY(near(robot(plant).tcp.x, 220));
        QVERIFY(robot(plant).joints.j2 < 0);
    }
    void gripFollowsTcpAndOnlyReleaseCounts() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(plant.setRobotGripper("r", true));
        QVERIFY(!robot(plant).carryingPart);
        QVERIFY(plant.setRobotGripper("r", false));
        QVERIFY(plant.moveRobotTcp("r", {220, 40, 20, 0}, 100));
        now += 100000;
        QVERIFY(plant.setRobotGripper("r", true));
        QVERIFY(robot(plant).carryingPart);
        QCOMPARE(robot(plant).partLocation, QString("gripper"));
        QVERIFY(!plant.restockRobot("r"));
        QVERIFY(plant.moveRobotTcp("r", {80, 200, 20, 0}, 100));
        now += 500;
        auto mid = robot(plant);
        QVERIFY(near(mid.partPose.x, mid.tcp.x));
        QVERIFY(near(mid.partPose.z, mid.tcp.z));
        now += 100000;
        QCOMPARE(robot(plant).okCount, qint64(0));
        QVERIFY(plant.setRobotGripper("r", false));
        QCOMPARE(robot(plant).okCount, qint64(1));
        QCOMPARE(robot(plant).partLocation, QString("ok"));
        QVERIFY(plant.setRobotGripper("r", false));
        QCOMPARE(robot(plant).okCount, qint64(1));
        QVERIFY(plant.restockRobot("r"));
        QCOMPARE(robot(plant).partLocation, QString("feed"));
        QVERIFY(plant.moveRobotTcp("r", {220, 40, 20, 0}, 100));
        now += 100000;
        QVERIFY(plant.setRobotGripper("r", true));
        QVERIFY(plant.moveRobotTcp("r", {180, 140, 100, 0}, 100));
        now += 100000;
        QVERIFY(plant.setRobotGripper("r", false));
        QCOMPARE(robot(plant).partLocation, QString("loose"));
        QCOMPARE(robot(plant).okCount, qint64(1));
        QVERIFY(!plant.startRobotSort("r", "ng"));
        QVERIFY(plant.restockRobot("r"));
    }
    void sortingConsumesEntireLargeClockJumpAndCountsOnce() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(!plant.startRobotSort("r", "invalid"));
        QVERIFY(plant.startRobotSort("r", "OK", 100));
        auto value = robot(plant);
        QVERIFY(value.programRunning);
        QCOMPARE(value.programSteps, 8);
        QCOMPARE(value.commandSequence, quint64(1));
        QVERIFY(!plant.moveRobotJoints("r", {}, 60));
        QVERIFY(!plant.setRobotGripper("r", false));
        QVERIFY(!plant.startRobotSort("r", "ng"));
        QVERIFY(!plant.teachRobotPoint("r", "busy"));
        QVERIFY(!plant.restockRobot("r"));
        now = 1000000;
        value = robot(plant);
        QVERIFY(!value.programRunning);
        QVERIFY(!value.moving);
        QCOMPARE(value.okCount, qint64(1));
        QCOMPARE(value.ngCount, qint64(0));
        QCOMPARE(value.completedSequence, quint64(1));
        QCOMPARE(value.partLocation, QString("ok"));
        QVERIFY(near(value.tcp.x, 80));
        QVERIFY(near(value.tcp.y, 200));
        QVERIFY(near(value.tcp.z, 100));
        now += 100000;
        QCOMPARE(robot(plant).okCount, qint64(1));
        QVERIFY(!plant.startRobotSort("r", "ng"));
        QVERIFY(plant.restockRobot("r"));
        QVERIFY(plant.startRobotSort("r", "ng", 100));
        now += 1000000;
        QCOMPARE(robot(plant).ngCount, qint64(1));
        QCOMPARE(robot(plant).partLocation, QString("ng"));
    }
    void stopInterlocksAndStopAllCancelWithoutResume() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r"), config("other")}));
        auto start = [&] { return plant.startRobotSort("r", "ok", 100); };
        QVERIFY(start());
        now += 100;
        QVERIFY(plant.stopRobot("r"));
        auto value = robot(plant);
        QCOMPARE(value.interruptedSequence, value.commandSequence);
        QVERIFY(!value.programRunning);
        const auto stopped = value.joints;
        now += 100000;
        QCOMPARE(robot(plant).joints.j1, stopped.j1);
        QVERIFY(start());
        now += 100;
        QVERIFY(plant.enableRobot("r", false));
        QVERIFY(plant.enableRobot("r", true));
        now += 100000;
        QVERIFY(!robot(plant).programRunning);
        QCOMPARE(robot(plant).completedSequence, quint64(0));
        QVERIFY(start());
        now += 100;
        QVERIFY(plant.setConnected("r", false));
        QVERIFY(plant.setConnected("r", true));
        now += 100000;
        QVERIFY(!robot(plant).programRunning);
        QVERIFY(start());
        now += 100;
        plant.setEmergencyStop(true);
        plant.setEmergencyStop(false);
        now += 100000;
        QVERIFY(!robot(plant).programRunning);
        QVERIFY(robot(plant).fault.isEmpty());
        QVERIFY(start());
        QVERIFY(plant.moveRobotJoints("other", {90, 90, 100, -90}, 100));
        now += 100;
        plant.stopAllAxes();
        now += 100000;
        QVERIFY(!robot(plant).programRunning);
        QVERIFY(!robot(plant, "other").moving);
        QCOMPARE(robot(plant, "other").interruptedSequence, quint64(1));
    }
    void interruptedHeldPartRemainsHeldUntilManualRelease() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(plant.startRobotSort("r", "ok", 100));
        now = 1000;
        QVERIFY(robot(plant).carryingPart);
        plant.setEmergencyStop(true);
        const auto halted = robot(plant);
        plant.setEmergencyStop(false);
        now = 100000;
        auto value = robot(plant);
        QVERIFY(value.carryingPart);
        QCOMPARE(value.partLocation, QString("gripper"));
        QCOMPARE(value.joints.z, halted.joints.z);
        QCOMPARE(value.okCount, qint64(0));
        QCOMPARE(value.interruptedSequence, quint64(1));
        QCOMPARE(value.completedSequence, quint64(0));
        QVERIFY(!plant.restockRobot("r"));
        QVERIFY(!plant.startRobotSort("r", "ng"));
        QVERIFY(plant.moveRobotTcp("r", {80, 200, 20, 0}, 100));
        now += 100000;
        QVERIFY(plant.setRobotGripper("r", false));
        value = robot(plant);
        QCOMPARE(value.commandSequence, quint64(3));
        QCOMPARE(value.completedSequence, quint64(3));
        QCOMPARE(value.okCount, qint64(1));
        QVERIFY(!value.carryingPart);
    }
    void gripperCommandsHaveTokensWithoutCompletingActiveMotion() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(plant.moveRobotJoints("r", {90, 0, 25, 0}, 100));
        QVERIFY(plant.setRobotGripper("r", true));
        auto value = robot(plant);
        QCOMPARE(value.commandSequence, quint64(2));
        QCOMPARE(value.completedSequence, quint64(2));
        QVERIFY(value.moving);
        now = 100;
        QVERIFY(plant.stopRobot("r"));
        value = robot(plant);
        QCOMPARE(value.interruptedSequence, quint64(1));
        QCOMPARE(value.completedSequence, quint64(2));
    }
    void teachingPlaybackIsBoundedAndImportsOnlyWhenStopped() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(!plant.playRobotTeaching("r"));
        QVERIFY(!plant.teachRobotPoint("r", "  "));
        QVERIFY(plant.teachRobotPoint("r", "home"));
        QVector<RobotWaypoint> points{{"a", {0, 90, 20, -90}, false}, {"b", {90, 0, 100, -90}, true}};
        QVERIFY(plant.setRobotTeaching("r", points));
        auto bad = points;
        bad[1].joints.z = 151;
        QVERIFY(!plant.setRobotTeaching("r", bad));
        QCOMPARE(robot(plant).taughtPoints[1].joints.z, 100.0);
        QVERIFY(!plant.removeRobotPoint("r", 2));
        QVERIFY(plant.playRobotTeaching("r", 100));
        QVERIFY(!plant.removeRobotPoint("r", 0));
        QVERIFY(!plant.setRobotTeaching("r", {}));
        now = 100000;
        auto value = robot(plant);
        QVERIFY(!value.programRunning);
        QCOMPARE(value.completedSequence, quint64(1));
        QCOMPARE(value.joints.j1, 90.0);
        QVERIFY(value.gripperClosed);
        QVERIFY(plant.removeRobotPoint("r", 0));
        QCOMPARE(robot(plant).taughtPoints.size(), 1);
        QVERIFY(plant.setRobotTeaching("r", QVector<RobotWaypoint>(100, points[0])));
        QVERIFY(!plant.teachRobotPoint("r", "overflow"));
        QVERIFY(!plant.setRobotTeaching("r", QVector<RobotWaypoint>(101, points[0])));
        QVERIFY(plant.moveRobotJoints("r", {}, 1));
        QVERIFY(!plant.setRobotTeaching("r", {}));
    }
    void resetPreservesTeachingAndRemovalDiscardsIt() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(plant.teachRobotPoint("r", "saved"));
        QVERIFY(plant.startRobotSort("r", "ok", 100));
        const auto before = plant.snapshot().interruptionGeneration;
        QVERIFY(plant.reset());
        QCOMPARE(plant.snapshot().interruptionGeneration, before + 1);
        QCOMPARE(robot(plant).taughtPoints.size(), 1);
        QCOMPARE(robot(plant).taughtPoints[0].name, QString("saved"));
        QCOMPARE(robot(plant).joints.z, 100.0);
        QCOMPARE(robot(plant).partLocation, QString("feed"));
        QCOMPARE(robot(plant).commandSequence, quint64(0));
        now = 1000000;
        QCOMPARE(robot(plant).okCount, qint64(0));
        QVERIFY(plant.configure({}));
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(robot(plant).taughtPoints.isEmpty());
    }
    void configureRetainsProgramsOnlyForSameIdAndCategoryAndBoundsTrail() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({config("r")}));
        QVERIFY(plant.startRobotSort("r", "ok", 100));
        now = 100;
        auto updated = config("r");
        updated.model = "updated";
        QVERIFY(plant.configure({updated}));
        QVERIFY(robot(plant).programRunning);
        now = 100000;
        QCOMPARE(robot(plant).okCount, qint64(1));
        QVERIFY(plant.configure({config("r", "motion")}));
        QVERIFY(plant.configure({config("r")}));
        QCOMPARE(robot(plant).okCount, qint64(0));
        QVERIFY(!robot(plant).programRunning);
        QVERIFY(plant.moveRobotJoints("r", {170, -170, 0, 180}, 1));
        for (int i = 0; i < 400; ++i) {
            now += 10;
            plant.snapshot();
        }
        QVERIFY(robot(plant).trail.size() <= 240);
        QVERIFY(robot(plant).trail.size() > 1);
        QVERIFY(plant.reset());
        QCOMPARE(robot(plant).trail.size(), 1);
        QCOMPARE(robot(plant).joints.j1, 0.0);
        now += 1000000;
        QVERIFY(!robot(plant).moving);
    }
};
QTEST_GUILESS_MAIN(RobotTests)
#include "tst_robot.moc"
