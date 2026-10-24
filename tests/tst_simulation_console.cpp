#include "infrastructure/SimulationPlant.h"
#include "ui/SimulationConsole.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QtTest>

namespace {
QList<ivs::DeviceConfig> devices() {
    return {{QStringLiteral("camera"), QStringLiteral("camera"), QStringLiteral("虚拟相机")},
            {QStringLiteral("motion"), QStringLiteral("motion"), QStringLiteral("虚拟平台")},
            {QStringLiteral("light"), QStringLiteral("light"), QStringLiteral("虚拟光源")},
            {QStringLiteral("plc"), QStringLiteral("plc"), QStringLiteral("虚拟 PLC")},
            {QStringLiteral("io"), QStringLiteral("io"), QStringLiteral("虚拟 I/O")},
            {QStringLiteral("real-plc"), QStringLiteral("plc"), QStringLiteral("真实 PLC"),
             QStringLiteral("modbus-tcp")}};
}
ivs::SimDeviceState state(const std::shared_ptr<ivs::SimulationPlant> &plant, const QString &id) {
    for (const auto &device : plant->snapshot().devices)
        if (device.id == id)
            return device;
    return {};
}
std::shared_ptr<ivs::SimulationPlant> configuredPlant() {
    auto plant = std::make_shared<ivs::SimulationPlant>();
    if (!plant->configure(devices()))
        qFatal("Simulation test fixture failed to configure");
    return plant;
}
} // namespace

class SimulationConsoleTests : public QObject {
    Q_OBJECT
  private slots:
    void exposesVirtualControls();
    void selectorsExcludeRealHardware();
    void lightControlsChangeFrameIllumination();
    void signalEditsReachEveryRegisterAndBitTable();
    void manualTriggerQueuesSelectedDevice();
    void axisTargetMovesAndRefreshPreservesDraft();
    void emergencyAndRunningPreventUnsafeReset();
    void disconnectedErrorsAreVisible();
    void focusedRegisterDraftSurvivesRefreshButNotAddressChange();
    void resetEditsClearsFocusedDrafts();
};

void SimulationConsoleTests::exposesVirtualControls() {
    ivs::SimulationConsole console({});
    QVERIFY2(console.findChild<QCheckBox *>(QStringLiteral("manualOverrideCheck")),
             "The console must expose manual virtual-hardware control");
}

void SimulationConsoleTests::selectorsExcludeRealHardware() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    auto *signalSelector = console.findChild<QComboBox *>(QStringLiteral("signalDeviceCombo"));
    QVERIFY(signalSelector);
    QCOMPARE(signalSelector->count(), 2);
    QCOMPARE(signalSelector->findData(QStringLiteral("real-plc")), -1);
    auto *connections = console.findChild<QComboBox *>(QStringLiteral("connectionDeviceCombo"));
    QVERIFY(connections);
    QCOMPARE(connections->count(), 5);
    QCOMPARE(connections->findData(QStringLiteral("real-plc")), -1);
}

void SimulationConsoleTests::lightControlsChangeFrameIllumination() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    auto *manual = console.findChild<QCheckBox *>(QStringLiteral("manualOverrideCheck"));
    auto *brightness = console.findChild<QSlider *>(QStringLiteral("light0Brightness"));
    auto *enabled = console.findChild<QCheckBox *>(QStringLiteral("light0Enabled"));
    QVERIFY(manual && brightness && enabled);
    manual->setChecked(true);
    QVERIFY(plant->snapshot().manualOverride);
    const QImage original = plant->renderFrame(QStringLiteral("camera"), QSize(256, 192), 0);
    QVERIFY(!original.isNull());
    brightness->setValue(180);
    QCOMPARE(state(plant, QStringLiteral("light")).lights.at(0).brightness, 180);
    const QImage bright = plant->renderFrame(QStringLiteral("camera"), QSize(256, 192), 0);
    QVERIFY(original != bright);
    enabled->setChecked(false);
    QVERIFY(!state(plant, QStringLiteral("light")).lights.at(0).enabled);
}

void SimulationConsoleTests::signalEditsReachEveryRegisterAndBitTable() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    auto *coil = console.findChild<QCheckBox *>(QStringLiteral("signal0Coil"));
    auto *discrete = console.findChild<QCheckBox *>(QStringLiteral("signal0Discrete"));
    auto *holding = console.findChild<QSpinBox *>(QStringLiteral("signal0Holding"));
    auto *input = console.findChild<QSpinBox *>(QStringLiteral("signal0Input"));
    QVERIFY(coil && discrete && holding && input);
    coil->setChecked(true);
    discrete->setChecked(true);
    holding->setValue(54321);
    input->setValue(12345);
    quint16 value = 0;
    QVERIFY(plant->readSignal(QStringLiteral("plc"), QStringLiteral("coil"), 0, &value));
    QCOMPARE(value, quint16(1));
    QVERIFY(plant->readSignal(QStringLiteral("plc"), QStringLiteral("discrete"), 0, &value));
    QCOMPARE(value, quint16(1));
    QVERIFY(plant->readSignal(QStringLiteral("plc"), QStringLiteral("holding"), 0, &value));
    QCOMPARE(value, quint16(54321));
    QVERIFY(plant->readSignal(QStringLiteral("plc"), QStringLiteral("input"), 0, &value));
    QCOMPARE(value, quint16(12345));
    auto *page = console.findChild<QComboBox *>(QStringLiteral("signalPageCombo"));
    QVERIFY(page);
    page->setCurrentIndex(3);
    auto *last = console.findChild<QSpinBox *>(QStringLiteral("signal63Holding"));
    QVERIFY(last);
    last->setValue(65535);
    QVERIFY(plant->readSignal(QStringLiteral("plc"), QStringLiteral("holding"), 63, &value));
    QCOMPARE(value, quint16(65535));
    QVERIFY(!console.findChild<QCheckBox *>(QStringLiteral("signal48Coil"))->isEnabled());
}

void SimulationConsoleTests::manualTriggerQueuesSelectedDevice() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    auto *automatic = console.findChild<QCheckBox *>(QStringLiteral("automaticTriggerCheck"));
    auto *trigger = console.findChild<QPushButton *>(QStringLiteral("triggerOnceButton"));
    QVERIFY(automatic && trigger);
    automatic->setChecked(false);
    QVERIFY(!plant->snapshot().automaticTrigger);
    trigger->click();
    trigger->click();
    QCOMPARE(state(plant, QStringLiteral("plc")).pendingTriggers, 2);
    QCOMPARE(state(plant, QStringLiteral("io")).pendingTriggers, 0);
    auto *pending = console.findChild<QLabel *>(QStringLiteral("pendingTriggerCount"));
    QVERIFY(pending);
    QVERIFY(pending->text().contains(QStringLiteral("2")));
}

void SimulationConsoleTests::axisTargetMovesAndRefreshPreservesDraft() {
    qint64 now = 0;
    const auto plant = std::make_shared<ivs::SimulationPlant>([&now] { return now; });
    QVERIFY(plant->configure(devices()));
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    auto *target = console.findChild<QDoubleSpinBox *>(QStringLiteral("axis0Target"));
    auto *speed = console.findChild<QDoubleSpinBox *>(QStringLiteral("axis0Speed"));
    auto *move = console.findChild<QPushButton *>(QStringLiteral("axis0Move"));
    QVERIFY(target && speed && move);
    target->setValue(10);
    speed->setValue(20);
    console.refresh();
    QCOMPARE(target->value(), 10.0);
    move->click();
    now = 1000;
    console.refresh();
    QCOMPARE(state(plant, QStringLiteral("motion")).axes.at(0).position, 10.0);
    auto *position = console.findChild<QLabel *>(QStringLiteral("axis0Position"));
    QVERIFY(position && position->text().contains(QStringLiteral("10.000")));
}

void SimulationConsoleTests::emergencyAndRunningPreventUnsafeReset() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    auto *reset = console.findChild<QPushButton *>(QStringLiteral("simulationResetButton"));
    auto *emergency = console.findChild<QPushButton *>(QStringLiteral("simulationEmergencyButton"));
    QVERIFY(reset && emergency);
    console.setRunning(true);
    QVERIFY(!reset->isEnabled());
    emergency->click();
    QVERIFY(plant->snapshot().emergencyStop);
    QVERIFY(!console.findChild<QPushButton *>(QStringLiteral("axis0Move"))->isEnabled());
    console.setRunning(false);
    QVERIFY(!reset->isEnabled());
    emergency->click();
    QVERIFY(!plant->snapshot().emergencyStop);
    QVERIFY(reset->isEnabled());
    QCOMPARE(state(plant, QStringLiteral("motion")).axes.at(0).moving, false);
}

void SimulationConsoleTests::disconnectedErrorsAreVisible() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    QSignalSpy messages(&console, &ivs::SimulationConsole::message);
    auto *target = console.findChild<QDoubleSpinBox *>(QStringLiteral("axis0Target"));
    auto *move = console.findChild<QPushButton *>(QStringLiteral("axis0Move"));
    QVERIFY(target && move);
    target->setValue(1000);
    move->click();
    QVERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().at(0).toString(), QStringLiteral("ERROR"));
    auto *notice = console.findChild<QLabel *>(QStringLiteral("simulationMessage"));
    QVERIFY(notice && !notice->text().isEmpty());
    const auto *connection = console.findChild<QComboBox *>(QStringLiteral("connectionDeviceCombo"));
    QVERIFY(connection);
    auto *connected = console.findChild<QCheckBox *>(QStringLiteral("simulationConnectedCheck"));
    QVERIFY(connected);
    connected->setChecked(false);
    QVERIFY(!state(plant, connection->currentData().toString()).connected);
}

void SimulationConsoleTests::focusedRegisterDraftSurvivesRefreshButNotAddressChange() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    console.resize(1300, 900);
    console.show();
    auto *holding = console.findChild<QSpinBox *>(QStringLiteral("signal0Holding"));
    auto *page = console.findChild<QComboBox *>(QStringLiteral("signalPageCombo"));
    QVERIFY(holding && page);
    auto *editor = holding->findChild<QLineEdit *>();
    QVERIFY(editor);
    console.activateWindow();
    editor->setFocus();
    QApplication::processEvents();
    QVERIFY(editor->hasFocus());
    editor->setText(QStringLiteral("777"));
    QVERIFY(plant->writeSignal(QStringLiteral("plc"), QStringLiteral("holding"), 0, 99));
    console.refresh();
    QCOMPARE(editor->text(), QStringLiteral("777"));
    page->setCurrentIndex(3);
    QCOMPARE(holding->objectName(), QStringLiteral("signal48Holding"));
    QCOMPARE(holding->text(), QStringLiteral("0"));
    QTest::keyClick(editor, Qt::Key_Return);
    quint16 value = 1;
    QVERIFY(plant->readSignal(QStringLiteral("plc"), QStringLiteral("holding"), 48, &value));
    QCOMPARE(value, quint16(0));
}

void SimulationConsoleTests::resetEditsClearsFocusedDrafts() {
    const auto plant = configuredPlant();
    ivs::SimulationConsole console(plant);
    console.setDevices(devices());
    console.resize(1300, 900);
    console.show();
    auto *target = console.findChild<QDoubleSpinBox *>(QStringLiteral("axis0Target"));
    auto *speed = console.findChild<QDoubleSpinBox *>(QStringLiteral("axis0Speed"));
    QVERIFY(target && speed);
    target->setValue(50);
    speed->setValue(2);
    auto *editor = target->findChild<QLineEdit *>();
    QVERIFY(editor);
    console.activateWindow();
    editor->setFocus();
    QApplication::processEvents();
    editor->setText(QStringLiteral("73"));
    QVERIFY(plant->reset());
    console.resetEdits();
    QCOMPARE(target->value(), 0.0);
    QCOMPARE(target->text(), QStringLiteral("0.000"));
    QCOMPARE(speed->value(), 25.0);
    // Destruction must also tolerate an uncommitted focused field.
    editor->setText(QStringLiteral("83"));
}

QTEST_MAIN(SimulationConsoleTests)
#include "tst_simulation_console.moc"
