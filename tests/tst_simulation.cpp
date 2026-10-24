#include "infrastructure/SimulationPlant.h"

#include <QTest>
#include <atomic>
#include <cmath>
#include <limits>
#include <thread>

using namespace ivs;

namespace {
DeviceConfig device(const QString &id, const QString &category,
                    const QString &backend = QStringLiteral("simulated")) {
    DeviceConfig value;
    value.id = id;
    value.category = category;
    value.backend = backend;
    value.model = QStringLiteral("simulation-test");
    return value;
}

SimDeviceState state(const SimulationPlant &plant, const QString &id) {
    for (const auto &value : plant.snapshot().devices)
        if (value.id == id)
            return value;
    return {};
}

QRect brightBounds(const QImage &image) {
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) > 128) {
                left = qMin(left, x);
                top = qMin(top, y);
                right = qMax(right, x);
                bottom = qMax(bottom, y);
            }
    return right < left ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
} // namespace

class SimulationTests : public QObject {
    Q_OBJECT
  private slots:
    void motionAdvancesAtSpeedAndStopsAtTarget() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({device("axis", "motion")}));
        QVERIFY(plant.moveAxis("axis", 0, 20, 10));
        QCOMPARE(state(plant, "axis").axes[0].position, 0.0);
        now = 500;
        QCOMPARE(state(plant, "axis").axes[0].position, 5.0);
        QVERIFY(state(plant, "axis").axes[0].moving);
        now = 2500;
        const auto axis = state(plant, "axis").axes[0];
        QCOMPARE(axis.position, 20.0);
        QVERIFY(!axis.moving);
        QVERIFY(plant.homeAxis("axis", 0, 4));
        QVERIFY(!state(plant, "axis").axes[0].homed);
        now = 3500;
        QCOMPARE(state(plant, "axis").axes[0].position, 16.0);
        now = 7500;
        QCOMPARE(state(plant, "axis").axes[0].position, 0.0);
        QVERIFY(state(plant, "axis").axes[0].homed);
    }

    void motionValidationNeverMovesAnotherDevice() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({device("a", "motion"), device("b", "motion"), device("lamp", "light")}));
        QString error;
        QVERIFY(!plant.moveAxis("missing", 0, 5, 10, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!plant.moveAxis("", 0, 5, 10));
        QVERIFY(!plant.moveAxis("lamp", 0, 5, 10));
        QVERIFY(!plant.moveAxis("a", -1, 5, 10));
        QVERIFY(!plant.moveAxis("a", 4, 5, 10));
        QVERIFY(!plant.moveAxis("a", 0, 101, 10));
        QVERIFY(!plant.moveAxis("a", 0, -101, 10));
        QVERIFY(!plant.moveAxis("a", 0, 10, 0));
        QVERIFY(!plant.moveAxis("a", 0, 10, -1));
        QVERIFY(!plant.moveAxis("a", 0, 10, 1001));
        QVERIFY(!plant.moveAxis("a", 0, std::numeric_limits<double>::quiet_NaN(), 10));
        QVERIFY(!plant.moveAxis("a", 0, 10, std::numeric_limits<double>::infinity()));
        QVERIFY(!plant.setAxisPosition("a", 0, 101));
        QVERIFY(plant.setAxisPosition("b", 1, -100));
        now = 1000;
        QCOMPARE(state(plant, "a").axes[0].position, 0.0);
        QCOMPARE(state(plant, "b").axes[1].position, -100.0);
        QVERIFY(plant.enableAxis("a", 0, false));
        QVERIFY(!plant.moveAxis("a", 0, 10, 10));
        QVERIFY(!plant.homeAxis("a", 0, 10));
    }

    void stopDisableDisconnectAndEmergencyCancelMotion() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({device("axis", "motion")}));
        QVERIFY(plant.moveAxis("axis", 0, 100, 20));
        now = 500;
        QVERIFY(plant.stopAxis("axis", 0));
        now = 5000;
        QCOMPARE(state(plant, "axis").axes[0].position, 10.0);
        QCOMPARE(state(plant, "axis").axes[0].target, 10.0);
        QVERIFY(plant.moveAxis("axis", 0, 100, 20));
        now = 5500;
        QVERIFY(plant.enableAxis("axis", 0, false));
        now = 6000;
        QCOMPARE(state(plant, "axis").axes[0].position, 20.0);
        QVERIFY(plant.enableAxis("axis", 0, true));
        QVERIFY(plant.moveAxis("axis", 0, 100, 20));
        now = 6500;
        QVERIFY(plant.setConnected("axis", false));
        QVERIFY(!plant.moveAxis("axis", 0, 50, 20));
        now = 7000;
        QCOMPARE(state(plant, "axis").axes[0].position, 30.0);
        QVERIFY(plant.setConnected("axis", true));
        QVERIFY(plant.moveAxis("axis", 0, 100, 20));
        now = 7500;
        plant.setEmergencyStop(true);
        QVERIFY(!plant.moveAxis("axis", 0, 50, 20));
        now = 8000;
        QCOMPARE(state(plant, "axis").axes[0].position, 40.0);
        QVERIFY(!state(plant, "axis").axes[0].moving);
        plant.setEmergencyStop(false);
        now = 9000;
        QCOMPARE(state(plant, "axis").axes[0].position, 40.0);
        QVERIFY(plant.moveAxis("axis", 0, 0, 20));
        QVERIFY(plant.moveAxis("axis", 1, 80, 20));
        now = 9500;
        plant.stopAllAxes();
        now = 20000;
        QCOMPARE(state(plant, "axis").axes[0].position, 30.0);
        QCOMPARE(state(plant, "axis").axes[1].position, 10.0);
    }

    void configurationRetainsOnlySameIdAndCategoryAtomically() {
        SimulationPlant plant;
        QVERIFY(plant.configure(
            {device("axis", "motion"), device("plc", "plc"), device("real", "plc", "modbus-tcp")}));
        QCOMPARE(plant.snapshot().devices.size(), 2);
        QVERIFY(plant.setAxisPosition("axis", 2, 7));
        QVERIFY(plant.writeSignal("plc", "holding", 3, 123));
        QVERIFY(plant.setConnected("axis", false));
        auto updated = device("axis", "motion");
        updated.model = "updated";
        updated.parameters = {{"tag", "updated"}};
        QVERIFY(plant.configure({updated, device("plc", "plc")}));
        QCOMPARE(state(plant, "axis").axes[2].position, 7.0);
        QCOMPARE(state(plant, "axis").model, QString("updated"));
        QVERIFY(!state(plant, "axis").connected);
        QString error;
        QVERIFY(!plant.configure({device("axis", "motion"), device("axis", "light")}, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(state(plant, "axis").axes[2].position, 7.0);
        QVERIFY(!plant.configure({device("", "motion")}));
        QVERIFY(!plant.configure({device("axis", "unknown")}));
        QVERIFY(plant.configure({device("axis", "light"), device("plc", "plc", "modbus-tcp")}));
        QCOMPARE(plant.snapshot().devices.size(), 1);
        QVERIFY(state(plant, "axis").axes.isEmpty());
        QCOMPARE(state(plant, "axis").lights[0].brightness, 80);
    }

    void signalTablesAreStrictAndDevicesIsolated() {
        SimulationPlant plant;
        QVERIFY(plant.configure({device("a", "plc"), device("b", "io"), device("cam", "camera")}));
        for (const QString table :
             {QString("holding"), QString("input"), QString("coil"), QString("discrete")}) {
            const bool bit = table == "coil" || table == "discrete";
            const int last = bit ? 15 : 63;
            const quint16 expected = bit ? 1 : 65535;
            QVERIFY(plant.writeSignal("a", table, last, expected));
            quint16 value = 9;
            QVERIFY(plant.readSignal("a", table, last, &value));
            QCOMPARE(value, expected);
            QVERIFY(plant.readSignal("b", table, last, &value));
            QCOMPARE(value, quint16(0));
            QVERIFY(!plant.writeSignal("a", table, last + 1, 1));
            QVERIFY(!plant.readSignal("a", table, -1, &value));
        }
        QVERIFY(!plant.writeSignal("a", "coil", 0, 2));
        QVERIFY(!plant.writeSignal("a", "discrete", 0, 65535));
        QVERIFY(!plant.writeSignal("a", "hold", 0, 1));
        QVERIFY(!plant.writeSignal("a", "Holding", 0, 1));
        QVERIFY(!plant.writeSignal("cam", "holding", 0, 1));
        QVERIFY(!plant.writeSignal("missing", "holding", 0, 1));
        QVERIFY(!plant.writeSignal("", "holding", 0, 1));
        QVERIFY(!plant.readSignal("a", "holding", 0, nullptr));
        QVERIFY(plant.setConnected("a", false));
        QVERIFY(!plant.writeSignal("a", "holding", 0, 1));
        quint16 value;
        QVERIFY(!plant.readSignal("a", "holding", 0, &value));
    }

    void triggerCreditsAreConsumedAndManualDiUsesCurrentLevel() {
        SimulationPlant plant;
        QVERIFY(plant.configure({device("plc", "plc"), device("io", "io"), device("lamp", "light")}));
        bool ready = false;
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(ready);
        plant.setAutomaticTrigger(false);
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(!ready);
        QVERIFY(plant.queueTrigger("plc"));
        QVERIFY(plant.queueTrigger("plc"));
        QCOMPARE(state(plant, "plc").pendingTriggers, 2);
        QVERIFY(plant.takeTrigger("io", &ready));
        QVERIFY(!ready);
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(ready);
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(ready);
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(!ready);
        QVERIFY(plant.writeSignal("plc", "discrete", 0, 1));
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(ready);
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(ready);
        QVERIFY(plant.writeSignal("plc", "discrete", 0, 0));
        QVERIFY(plant.takeTrigger("plc", &ready));
        QVERIFY(!ready);
        QVERIFY(!plant.queueTrigger("lamp"));
        QVERIFY(!plant.takeTrigger("missing", &ready));
        QVERIFY(!plant.takeTrigger("plc", nullptr));
        plant.setEmergencyStop(true);
        QVERIFY(!plant.takeTrigger("plc", &ready));
        plant.setEmergencyStop(false);
        QVERIFY(plant.setConnected("plc", false));
        QVERIFY(!plant.takeTrigger("plc", &ready));
        QVERIFY(plant.setConnected("plc", true));
        int accepted = 0;
        while (accepted < 100000 && plant.queueTrigger("plc"))
            ++accepted;
        QVERIFY(accepted > 0);
        QVERIFY(accepted < 100000);
        QCOMPARE(state(plant, "plc").pendingTriggers, accepted);
    }

    void interruptionGenerationSurvivesFaultClearAndReset() {
        SimulationPlant plant;
        QVERIFY(plant.configure({device("axis", "motion"), device("lamp", "light")}));
        const quint64 initial = plant.snapshot().interruptionGeneration;
        QVERIFY(plant.setLight("lamp", 0, true, 20));
        QVERIFY(plant.setConnected("axis", true));
        QCOMPARE(plant.snapshot().interruptionGeneration, initial);
        plant.setEmergencyStop(true);
        QCOMPARE(plant.snapshot().interruptionGeneration, initial + 1);
        plant.setEmergencyStop(true);
        plant.setEmergencyStop(false);
        QCOMPARE(plant.snapshot().interruptionGeneration, initial + 1);
        QVERIFY(plant.setConnected("axis", false));
        QCOMPARE(plant.snapshot().interruptionGeneration, initial + 2);
        QVERIFY(plant.setConnected("axis", false));
        QVERIFY(plant.setConnected("axis", true));
        QCOMPARE(plant.snapshot().interruptionGeneration, initial + 2);
        QVERIFY(plant.reset());
        QCOMPARE(plant.snapshot().interruptionGeneration, initial + 3);
        QVERIFY(plant.reset());
        QCOMPARE(plant.snapshot().interruptionGeneration, initial + 4);
        QVERIFY(plant.configure({device("axis", "motion")}));
        QVERIFY(!plant.setConnected("missing", false));
        QCOMPARE(plant.snapshot().interruptionGeneration, initial + 4);
    }

    void resetClearsSignalsMotionSceneAndGlobalModes() {
        qint64 now = 0;
        SimulationPlant plant([&] { return now; });
        QVERIFY(plant.configure({device("axis", "motion"), device("lamp", "light"), device("plc", "plc")}));
        QVERIFY(plant.moveAxis("axis", 0, 100, 20));
        QVERIFY(plant.enableAxis("axis", 1, false));
        QVERIFY(plant.setLight("lamp", 0, false, 9));
        QVERIFY(plant.writeSignal("plc", "holding", 63, 8));
        QVERIFY(plant.writeSignal("plc", "discrete", 0, 1));
        QVERIFY(plant.queueTrigger("plc"));
        QVERIFY(plant.setConnected("plc", false));
        plant.setAutomaticTrigger(false);
        plant.setManualOverride(true);
        plant.setEmergencyStop(true);
        QVERIFY(plant.setPartMode("missing"));
        QVERIFY(plant.setPixelsPerMm(8));
        const auto revision = plant.snapshot().revision;
        QVERIFY(plant.reset());
        now = 10000;
        const auto snapshot = plant.snapshot();
        QVERIFY(snapshot.revision > revision);
        QVERIFY(snapshot.automaticTrigger);
        QVERIFY(!snapshot.manualOverride);
        QVERIFY(!snapshot.emergencyStop);
        QCOMPARE(snapshot.partMode, QString("alternating"));
        QCOMPARE(snapshot.pixelsPerMm, 20.0);
        const auto axis = state(plant, "axis").axes;
        QCOMPARE(axis.size(), 4);
        QCOMPARE(axis[0].position, 0.0);
        QCOMPARE(axis[0].target, 0.0);
        QVERIFY(!axis[0].moving);
        QVERIFY(axis[1].enabled);
        QVERIFY(axis[0].homed);
        QCOMPARE(axis[0].minimum, -100.0);
        QCOMPARE(axis[0].maximum, 100.0);
        QCOMPARE(state(plant, "lamp").lights[0].brightness, 80);
        QVERIFY(state(plant, "lamp").lights[0].enabled);
        QVERIFY(state(plant, "plc").connected);
        QCOMPARE(state(plant, "plc").holdingRegisters[63], quint16(0));
        QVERIFY(!state(plant, "plc").discreteInputs[0]);
        QCOMPARE(state(plant, "plc").pendingTriggers, 0);
    }

    void renderedImageUsesMotionLightingAndPartMode() {
        SimulationPlant plant;
        QVERIFY(
            plant.configure({device("cam", "camera"), device("axis", "motion"), device("lamp", "light")}));
        QString error;
        const auto normal = plant.renderFrame("cam", QSize(640, 640), 1, &error);
        QVERIFY2(!normal.isNull(), qPrintable(error));
        QCOMPARE(brightBounds(normal), QRect(116, 118, 400, 400));
        QCOMPARE(qGray(normal.pixel(320, 320)), 236);
        QCOMPARE(brightBounds(plant.renderFrame("cam", QSize(640, 640), 5)).width(), 460);
        QVERIFY(plant.setAxisPosition("axis", 0, 2));
        QVERIFY(plant.setAxisPosition("axis", 1, -1));
        const auto shifted = brightBounds(plant.renderFrame("cam", QSize(640, 640), 1));
        QCOMPARE(shifted, QRect(156, 98, 400, 400));
        QVERIFY(plant.setPixelsPerMm(10));
        QCOMPARE(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)), QRect(136, 108, 400, 400));
        QVERIFY(plant.setLight("lamp", 0, true, 20));
        const auto dim = plant.renderFrame("cam", QSize(640, 640), 1);
        QCOMPARE(qGray(dim.pixel(320, 320)), 59);
        QVERIFY(brightBounds(dim).isEmpty());
        QVERIFY(plant.setLight("lamp", 0, false, 255));
        QVERIFY(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)).isEmpty());
        QVERIFY(plant.setLight("lamp", 0, true, 255));
        QCOMPARE(qGray(plant.renderFrame("cam", QSize(640, 640), 1).pixel(320, 320)), 255);
        QVERIFY(plant.setPartMode("good"));
        QCOMPARE(brightBounds(plant.renderFrame("cam", QSize(640, 640), 5)).width(), 400);
        QVERIFY(plant.setPartMode("ng"));
        QCOMPARE(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)).width(), 460);
        QVERIFY(plant.setPartMode("missing"));
        QVERIFY(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)).isEmpty());
        QVERIFY(!plant.setPartMode("arbitrary"));
        QVERIFY(!plant.setPixelsPerMm(0));
        QVERIFY(!plant.setPixelsPerMm(std::numeric_limits<double>::infinity()));
        QVERIFY(!plant.setLight("lamp", -1, true, 80));
        QVERIFY(!plant.setLight("lamp", 4, true, 80));
        QVERIFY(!plant.setLight("lamp", 0, true, 256));
        QVERIFY(!plant.setLight("lamp", 0, true, -1));
        QVERIFY(!plant.setLight("axis", 0, true, 80));
    }

    void renderReferencesValidateAndOffscreenPartCropsNaturally() {
        SimulationPlant plant;
        auto cam = device("cam", "camera");
        cam.parameters = {{"motionDeviceId", "b"}, {"lightDeviceId", "l2"}, {"lightChannel", 2}};
        const QList<DeviceConfig> configs{cam, device("a", "motion"), device("b", "motion"),
                                          device("l1", "light"), device("l2", "light")};
        QVERIFY(plant.configure(configs));
        QVERIFY(plant.setAxisPosition("a", 0, -50));
        QVERIFY(plant.setAxisPosition("b", 0, 1));
        QVERIFY(plant.setLight("l1", 0, false, 0));
        QCOMPARE(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)), QRect(136, 118, 400, 400));
        QVERIFY(plant.setLight("l2", 2, true, 20));
        QCOMPARE(qGray(plant.renderFrame("cam", QSize(640, 640), 1).pixel(320, 320)), 59);
        QVERIFY(plant.setLight("l2", 2, true, 80));
        QVERIFY(plant.setAxisPosition("b", 0, 20));
        // At sensor x=639 the circle's dx=-76.5, so its visible dy extends to +/-184.5.
        QCOMPARE(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)), QRect(516, 133, 124, 370));
        QVERIFY(plant.setAxisPosition("b", 0, 100));
        QVERIFY(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)).isEmpty());
        QVERIFY(plant.setPixelsPerMm(std::numeric_limits<double>::max()));
        QVERIFY(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)).isEmpty());
        QVERIFY(plant.setAxisPosition("b", 0, -100));
        QVERIFY(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)).isEmpty());
        QVERIFY(plant.setAxisPosition("b", 0, 0));
        QCOMPARE(brightBounds(plant.renderFrame("cam", QSize(640, 640), 1)), QRect(116, 118, 400, 400));
        QString error;
        QVERIFY(plant.renderFrame("missing", QSize(640, 640), 1, &error).isNull());
        QVERIFY(!error.isEmpty());
        QVERIFY(plant.renderFrame("", QSize(640, 640), 1).isNull());
        QVERIFY(plant.renderFrame("a", QSize(640, 640), 1).isNull());
        QVERIFY(plant.renderFrame("cam", QSize(15, 640), 1).isNull());
        QVERIFY(plant.renderFrame("cam", QSize(8192, 8192), 1).isNull());
        QVERIFY(plant.setConnected("b", false));
        QVERIFY(plant.renderFrame("cam", QSize(640, 640), 1, &error).isNull());
        QVERIFY(error.contains("b"));
        QVERIFY(plant.setConnected("b", true));
        QVERIFY(plant.setConnected("l2", false));
        QVERIFY(plant.renderFrame("cam", QSize(640, 640), 1).isNull());
        QVERIFY(plant.setConnected("l2", true));
        QVERIFY(plant.setConnected("cam", false));
        QVERIFY(plant.renderFrame("cam", QSize(640, 640), 1).isNull());
        for (const QJsonObject parameters :
             {QJsonObject{{"motionDeviceId", "missing"}}, QJsonObject{{"motionDeviceId", "l1"}},
              QJsonObject{{"lightDeviceId", "a"}}, QJsonObject{{"lightDeviceId", ""}},
              QJsonObject{{"lightChannel", 1.5}}, QJsonObject{{"motionDeviceId", 12}}}) {
            cam.parameters = parameters;
            QVERIFY(plant.configure({cam, device("a", "motion"), device("l1", "light")}));
            QVERIFY(plant.setConnected("cam", true));
            QVERIFY(plant.renderFrame("cam", QSize(640, 640), 1, &error).isNull());
            QVERIFY(!error.isEmpty());
        }
    }

    void concurrentSignalUpdatesAndSnapshotsRemainCoherent() {
        SimulationPlant plant;
        QVERIFY(plant.configure({device("plc", "plc")}));
        std::atomic<bool> succeeded{true};
        std::thread writer([&] {
            for (int i = 0; i < 2000; ++i)
                if (!plant.writeSignal("plc", "holding", 0, quint16(i)))
                    succeeded = false;
        });
        for (int i = 0; i < 2000; ++i) {
            const auto snapshot = plant.snapshot();
            QCOMPARE(snapshot.devices.size(), 1);
            QCOMPARE(snapshot.devices[0].holdingRegisters.size(), 64);
        }
        writer.join();
        QVERIFY(succeeded.load());
        quint16 value = 0;
        QVERIFY(plant.readSignal("plc", "holding", 0, &value));
        QCOMPARE(value, quint16(1999));
    }
};

QTEST_GUILESS_MAIN(SimulationTests)
#include "tst_simulation.moc"
