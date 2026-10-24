#include "infrastructure/SimulationPlant.h"
#include "ui/RobotConsole.h"
#include "ui/RobotScene.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QList<ivs::DeviceConfig> devices() {
    return {{QStringLiteral("scara"), QStringLiteral("robot"), QStringLiteral("SCARA-4"),
             QStringLiteral("simulated")}};
}
ivs::SimRobotState robot(const std::shared_ptr<ivs::SimulationPlant> &plant) {
    return plant->snapshot().devices.first().robot;
}
template <typename T> T *control(ivs::RobotConsole &console, const char *name) {
    return console.findChild<T *>(QString::fromLatin1(name));
}
QImage rendered(ivs::RobotScene *scene) {
    return scene->grab().toImage().convertToFormat(QImage::Format_RGB32);
}
} // namespace

class RobotConsoleTests : public QObject {
    Q_OBJECT
  private slots:
    void emptyStateRequestsSimulatedRobot();
    void targetsCommandRealPlantAndPreserveDrafts();
    void sceneClickEditsXYWithoutMoving();
    void jointsAndGripperTeachActualState();
    void teachingFilesRoundTripAndRejectAtomically();
    void runningAndProgramsLockConflictingActions();
    void focusedDraftIsResetAndTeardownIsSafe();
    void sortRestockAndPlaybackUseSharedPlant();
    void selectorPreservesPlantAndRebindsOnReset();
    void deviceLabelTracksModelEditsWithoutDiscardingDraft();
    void sceneShowsActiveJointAndProgramTargetsWithoutReplacingDraft();
    void idleElbowSelectionChangesGhostWithoutMoving();
};

void RobotConsoleTests::emptyStateRequestsSimulatedRobot() {
    auto plant = std::make_shared<ivs::SimulationPlant>();
    ivs::RobotConsole console(plant, QString());
    auto *add = console.findChild<QPushButton *>(QStringLiteral("robotAddDeviceButton"));
    QVERIFY2(add, "Empty laboratory must offer adding a simulated SCARA device");
    QSignalSpy request(&console, &ivs::RobotConsole::addRobotRequested);
    add->click();
    QCOMPARE(request.count(), 1);
}

void RobotConsoleTests::targetsCommandRealPlantAndPreserveDrafts() {
    qint64 now = 0;
    auto plant = std::make_shared<ivs::SimulationPlant>([&now] { return now; });
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    auto *x = control<QDoubleSpinBox>(console, "robotXTarget");
    auto *y = control<QDoubleSpinBox>(console, "robotYTarget");
    auto *z = control<QDoubleSpinBox>(console, "robotZTarget");
    auto *r = control<QDoubleSpinBox>(console, "robotRTarget");
    auto *move = control<QPushButton>(console, "robotMoveTcp");
    QVERIFY(x && y && z && r && move);
    x->setValue(220);
    y->setValue(40);
    z->setValue(20);
    r->setValue(0);
    console.refresh();
    QCOMPARE(x->value(), 220.0);
    QCOMPARE(z->value(), 20.0);
    QVERIFY(!robot(plant).moving);
    move->click();
    QVERIFY(robot(plant).moving);
    now += 30000;
    console.refresh();
    QVERIFY(std::abs(robot(plant).tcp.x - 220) < 0.001);
    QVERIFY(std::abs(robot(plant).tcp.y - 40) < 0.001);
    QCOMPARE(robot(plant).tcp.z, 20.0);
    QVERIFY(control<QLabel>(console, "robotTcpLabel")->text().contains(QStringLiteral("220.0")));
    x->setValue(315);
    y->setValue(315);
    QSignalSpy messages(&console, &ivs::RobotConsole::message);
    move->click();
    QVERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().at(0).toString(), QStringLiteral("ERROR"));
    QVERIFY(!robot(plant).moving);
}

void RobotConsoleTests::sceneClickEditsXYWithoutMoving() {
    auto plant = std::make_shared<ivs::SimulationPlant>();
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    auto *scene = control<ivs::RobotScene>(console, "robotScene");
    auto *z = control<QDoubleSpinBox>(console, "robotZTarget");
    auto *r = control<QDoubleSpinBox>(console, "robotRTarget");
    QVERIFY(scene && z && r);
    z->setValue(63);
    r->setValue(42);
    QSignalSpy selected(scene, &ivs::RobotScene::targetRequested);
    const QPoint click = scene->project({200, 50, 0, 0}).toPoint();
    QTest::mouseClick(scene, Qt::LeftButton, Qt::NoModifier, click);
    QCOMPARE(selected.count(), 1);
    QVERIFY(std::abs(control<QDoubleSpinBox>(console, "robotXTarget")->value() - 200) < 3);
    QVERIFY(std::abs(control<QDoubleSpinBox>(console, "robotYTarget")->value() - 50) < 3);
    QCOMPARE(z->value(), 63.0);
    QCOMPARE(r->value(), 42.0);
    QVERIFY(!robot(plant).moving);
}

void RobotConsoleTests::jointsAndGripperTeachActualState() {
    qint64 now = 0;
    auto plant = std::make_shared<ivs::SimulationPlant>([&now] { return now; });
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    auto *j1 = control<QDoubleSpinBox>(console, "robotJ1Target");
    auto *j2 = control<QDoubleSpinBox>(console, "robotJ2Target");
    auto *j4 = control<QDoubleSpinBox>(console, "robotJ4Target");
    QVERIFY(j1 && j2 && j4);
    j1->setValue(30);
    j2->setValue(60);
    j4->setValue(-90);
    control<QPushButton>(console, "robotMoveJoints")->click();
    now += 30000;
    console.refresh();
    QCOMPARE(robot(plant).joints.j1, 30.0);
    QCOMPARE(robot(plant).joints.j2, 60.0);
    control<QPushButton>(console, "robotGripClose")->click();
    QVERIFY(robot(plant).gripperClosed);
    j1->setValue(-60); // A draft must not become the taught position.
    control<QLineEdit>(console, "robotTeachName")->setText(QStringLiteral("起始姿态"));
    control<QPushButton>(console, "robotTeachButton")->click();
    const auto points = robot(plant).taughtPoints;
    QCOMPARE(points.size(), 1);
    QCOMPARE(points.first().name, QStringLiteral("起始姿态"));
    QCOMPARE(points.first().joints.j1, 30.0);
    QVERIFY(points.first().gripperClosed);
    QCOMPARE(control<QTableWidget>(console, "robotTeachTable")->rowCount(), 1);
}

void RobotConsoleTests::teachingFilesRoundTripAndRejectAtomically() {
    auto plant = std::make_shared<ivs::SimulationPlant>();
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    QVERIFY(plant->teachRobotPoint(QStringLiteral("scara"), QStringLiteral("P1")));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("teaching.json"));
    QString error;
    QVERIFY2(console.saveTeaching(path, &error), qPrintable(error));
    QVERIFY(plant->setRobotTeaching(QStringLiteral("scara"), {}));
    QVERIFY2(console.loadTeaching(path, &error), qPrintable(error));
    QCOMPARE(robot(plant).taughtPoints.size(), 1);
    QFile broken(path);
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("{\"schema\":1,\"points\":[{\"name\":\"bad\",\"joints\":{\"j1\":999,\"j2\":90,\"z\":100,"
                 "\"j4\":-90},\"gripper\":false}]}");
    broken.close();
    QVERIFY(!console.loadTeaching(path, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(robot(plant).taughtPoints.size(), 1);
    QCOMPARE(robot(plant).taughtPoints.first().name, QStringLiteral("P1"));
    console.setRunning(true);
    QVERIFY(!console.loadTeaching(path, &error));
    QCOMPARE(robot(plant).taughtPoints.size(), 1);
}

void RobotConsoleTests::runningAndProgramsLockConflictingActions() {
    qint64 now = 0;
    auto plant = std::make_shared<ivs::SimulationPlant>([&now] { return now; });
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    const QStringList locked{QStringLiteral("robotMoveTcp"),     QStringLiteral("robotMoveJoints"),
                             QStringLiteral("robotHomeButton"),  QStringLiteral("robotGripOpen"),
                             QStringLiteral("robotGripClose"),   QStringLiteral("robotRestockButton"),
                             QStringLiteral("robotTeachButton"), QStringLiteral("robotPlayButton"),
                             QStringLiteral("robotSortOk"),      QStringLiteral("robotSortNg")};
    console.setRunning(true);
    for (const auto &name : locked)
        QVERIFY2(!console.findChild<QPushButton *>(name)->isEnabled(), qPrintable(name));
    QVERIFY(!control<QCheckBox>(console, "robotEnabledCheck")->isEnabled());
    QVERIFY(control<QPushButton>(console, "robotStopButton")->isEnabled());
    console.setRunning(false);
    QVERIFY(plant->startRobotSort(QStringLiteral("scara"), QStringLiteral("OK")));
    console.refresh();
    for (const auto &name : locked)
        QVERIFY2(!console.findChild<QPushButton *>(name)->isEnabled(), qPrintable(name));
    control<QPushButton>(console, "robotStopButton")->click();
    QVERIFY(!robot(plant).programRunning);
    auto *emergency = control<QPushButton>(console, "robotEmergencyButton");
    QVERIFY(emergency);
    emergency->click();
    QVERIFY(plant->snapshot().emergencyStop);
    QVERIFY(!control<QPushButton>(console, "robotMoveTcp")->isEnabled());
    emergency->click();
    QVERIFY(!plant->snapshot().emergencyStop);
    QVERIFY(!robot(plant).programRunning);
}

void RobotConsoleTests::focusedDraftIsResetAndTeardownIsSafe() {
    auto plant = std::make_shared<ivs::SimulationPlant>();
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    console.resize(1200, 820);
    console.show();
    auto *x = control<QDoubleSpinBox>(console, "robotXTarget");
    auto *edit = x->findChild<QLineEdit *>();
    QVERIFY(edit);
    console.activateWindow();
    edit->setFocus();
    QApplication::processEvents();
    edit->setText(QStringLiteral("273"));
    const QString draft = edit->text();
    console.refresh();
    QCOMPARE(edit->text(), draft);
    console.resetEdits();
    QCOMPARE(x->value(), 180.0);
    QVERIFY(x->text().startsWith(QStringLiteral("180.0")));
    edit->setText(QStringLiteral("283"));
}

void RobotConsoleTests::sortRestockAndPlaybackUseSharedPlant() {
    qint64 now = 0;
    auto plant = std::make_shared<ivs::SimulationPlant>([&now] { return now; });
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    control<QPushButton>(console, "robotSortOk")->click();
    QVERIFY(robot(plant).programRunning);
    now += 60000;
    console.refresh();
    QCOMPARE(robot(plant).okCount, qint64(1));
    QCOMPARE(robot(plant).ngCount, qint64(0));
    QCOMPARE(robot(plant).partLocation, QStringLiteral("ok"));
    control<QPushButton>(console, "robotRestockButton")->click();
    QCOMPARE(robot(plant).partLocation, QStringLiteral("feed"));
    QCOMPARE(robot(plant).okCount, qint64(1));
    control<QPushButton>(console, "robotSortNg")->click();
    now += 60000;
    console.refresh();
    QCOMPARE(robot(plant).okCount, qint64(1));
    QCOMPARE(robot(plant).ngCount, qint64(1));
    const QVector<ivs::RobotWaypoint> points{{QStringLiteral("P1"), {10, 80, 60, -90}, true},
                                             {QStringLiteral("P2"), {20, 70, 80, -90}, false}};
    QVERIFY(plant->setRobotTeaching(QStringLiteral("scara"), points));
    console.refresh();
    control<QPushButton>(console, "robotPlayButton")->click();
    QVERIFY(robot(plant).programRunning);
    now += 60000;
    console.refresh();
    QCOMPARE(robot(plant).joints.j1, 20.0);
    QCOMPARE(robot(plant).joints.z, 80.0);
    QVERIFY(!robot(plant).gripperClosed);
    auto *table = control<QTableWidget>(console, "robotTeachTable");
    table->selectRow(0);
    control<QPushButton>(console, "robotMovePoint")->click();
    now += 60000;
    console.refresh();
    QCOMPARE(robot(plant).joints.j1, 10.0);
    control<QPushButton>(console, "robotRemovePoint")->click();
    QCOMPARE(robot(plant).taughtPoints.size(), 1);
    QCOMPARE(robot(plant).taughtPoints.first().name, QStringLiteral("P2"));
}

void RobotConsoleTests::selectorPreservesPlantAndRebindsOnReset() {
    qint64 now = 0;
    auto plant = std::make_shared<ivs::SimulationPlant>([&now] { return now; });
    auto list = devices();
    list.append({QStringLiteral("second"), QStringLiteral("robot"), QStringLiteral("SCARA-4"),
                 QStringLiteral("simulated")});
    list.append({QStringLiteral("real"), QStringLiteral("robot"), QStringLiteral("Vendor"),
                 QStringLiteral("hardware")});
    QVERIFY(plant->configure(list));
    QVERIFY(plant->moveRobotJoints(QStringLiteral("scara"), {20, 60, 70, -80}, 60));
    now = 10000;
    ivs::RobotConsole console(plant, QString());
    console.setDevices(list);
    QCOMPARE(robot(plant).joints.j1, 20.0);
    auto *selector = control<QComboBox>(console, "robotDeviceCombo");
    QCOMPARE(selector->count(), 2);
    auto *j1 = control<QDoubleSpinBox>(console, "robotJ1Target");
    QCOMPARE(j1->value(), 20.0);
    j1->setValue(42);
    console.setDevices(list);
    QCOMPARE(j1->value(), 42.0);
    selector->setCurrentIndex(1);
    QCOMPARE(j1->value(), 0.0);
    j1->setValue(60);
    control<QPushButton>(console, "robotMoveJoints")->click();
    now += 10000;
    console.refresh();
    const auto state = plant->snapshot();
    QCOMPARE(state.devices.at(0).robot.joints.j1, 20.0);
    QCOMPARE(state.devices.at(1).robot.joints.j1, 60.0);
    j1->setValue(55);
    QVERIFY(plant->reset());
    console.refresh();
    QCOMPARE(j1->value(), 0.0);
    control<QCheckBox>(console, "robotOnlineCheck")->setChecked(false);
    QVERIFY(!plant->snapshot().devices.at(1).connected);
    QVERIFY(!control<QPushButton>(console, "robotMoveJoints")->isEnabled());
    QVERIFY(control<QPushButton>(console, "robotStopButton")->isEnabled());
}

void RobotConsoleTests::deviceLabelTracksModelEditsWithoutDiscardingDraft() {
    auto plant = std::make_shared<ivs::SimulationPlant>();
    auto list = devices();
    QVERIFY(plant->configure(list));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(list);
    auto *x = control<QDoubleSpinBox>(console, "robotXTarget");
    x->setValue(240);
    list.first().model = QStringLiteral("SCARA renamed");
    console.setDevices(list);
    QVERIFY(
        control<QComboBox>(console, "robotDeviceCombo")->currentText().contains(QStringLiteral("renamed")));
    QCOMPARE(x->value(), 240.0);
}

void RobotConsoleTests::sceneShowsActiveJointAndProgramTargetsWithoutReplacingDraft() {
    qint64 now = 0;
    auto plant = std::make_shared<ivs::SimulationPlant>([&now] { return now; });
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    console.resize(1100, 800);
    console.show();
    QApplication::processEvents();
    auto *scene = control<ivs::RobotScene>(console, "robotScene");
    auto *x = control<QDoubleSpinBox>(console, "robotXTarget");
    auto *y = control<QDoubleSpinBox>(console, "robotYTarget");
    auto *z = control<QDoubleSpinBox>(console, "robotZTarget");
    x->setValue(240);
    y->setValue(50);
    z->setValue(60);
    // Straight joints have the independently known TCP (320, 0, 80).
    QVERIFY(plant->moveRobotJoints(QStringLiteral("scara"), {0, 0, 80, 0}, 60));
    console.refresh();
    ivs::RobotScene reference;
    reference.resize(scene->size());
    reference.setState(robot(plant), true, false);
    reference.setTarget({320, 0, 80, 0});
    QCOMPARE(rendered(scene), rendered(&reference));
    x->setValue(250); // Editing during a joint move retains the active marker.
    QCOMPARE(rendered(scene), rendered(&reference));
    now += 30000;
    console.refresh();
    reference.setState(robot(plant), true, false);
    reference.setTarget({250, 50, 60, 0});
    QCOMPARE(rendered(scene), rendered(&reference));
    QVERIFY(plant->startRobotSort(QStringLiteral("scara"), QStringLiteral("OK")));
    console.setRunning(true);
    reference.setState(robot(plant), true, false);
    reference.setTarget({220, 40, 100, 0});
    QCOMPARE(rendered(scene), rendered(&reference));
    QCOMPARE(x->value(), 250.0);
    QCOMPARE(y->value(), 50.0);
    QCOMPARE(z->value(), 60.0);
}

void RobotConsoleTests::idleElbowSelectionChangesGhostWithoutMoving() {
    auto plant = std::make_shared<ivs::SimulationPlant>();
    QVERIFY(plant->configure(devices()));
    ivs::RobotConsole console(plant, QString());
    console.setDevices(devices());
    auto *scene = control<ivs::RobotScene>(console, "robotScene");
    auto *elbow = control<QComboBox>(console, "robotElbowCombo");
    QVERIFY(scene && elbow);
    scene->setTopView(true);
    const QImage positive = scene->grab().toImage();
    elbow->setCurrentIndex(1);
    QVERIFY2(scene->grab().toImage() != positive,
             "Changing the chosen IK branch must immediately change the target ghost");
    ivs::RobotScene reference;
    reference.resize(scene->size());
    reference.setState(robot(plant), true, false);
    reference.setTopView(true);
    reference.setTarget({180, 140, 100, 0}, false);
    QCOMPARE(rendered(scene), rendered(&reference));
    QVERIFY(!robot(plant).moving);
    QCOMPARE(robot(plant).joints.j2, 90.0);
    QCOMPARE(control<QDoubleSpinBox>(console, "robotXTarget")->value(), 180.0);
    elbow->setCurrentIndex(0);
    QCOMPARE(scene->grab().toImage(), positive);
}

QTEST_MAIN(RobotConsoleTests)
#include "tst_robot_console.moc"
