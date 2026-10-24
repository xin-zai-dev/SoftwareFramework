#include "domain/ImageAnalysis.h"
#include "domain/Project.h"
#include <QJsonArray>
#include <QPainter>
#include <QTemporaryDir>
#include <QtTest>

using namespace ivs;
class DomainTest : public QObject {
    Q_OBJECT
  private slots:
    void rejectsUnsafeResolutionAndCycle() {
        auto p = defaultProject();
        p.resolution = QSize(100000, 100000);
        p.cycleMs = 0;
        QVERIFY(!validateProject(p).isEmpty());
    }
    void rejectsStoreBeforeCaptureAndDuplicateIds() {
        auto p = defaultProject();
        p.flow = {{"same", "store", "保存", true, {}}, {"same", "capture", "采集", true, {}}};
        QVERIFY(!validateProject(p).isEmpty());
    }
    void savesAndLoadsRealProject() {
        QTemporaryDir dir;
        auto p = defaultProject();
        p.name = QStringLiteral("检测工程 Ω");
        QString error;
        QVERIFY2(validateProject(p).isEmpty(), qPrintable(validateProject(p).join(";")));
        QVERIFY2(saveProject(p, dir.filePath("sample.ivproj"), &error), qPrintable(error));
        ProjectConfig loaded;
        QVERIFY2(loadProject(dir.filePath("sample.ivproj"), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.name, p.name);
        QCOMPARE(loaded.resolution, QSize(2448, 2048));
        QCOMPARE(loaded.flow.size(), p.flow.size());
        QCOMPARE(loaded.panels.front().field, QStringLiteral("diameterMm"));
    }
    void invalidFileDoesNotOverwriteDestination() {
        auto p = defaultProject();
        p.name = QStringLiteral("保留原工程");
        QString error;
        QVERIFY(!projectFromJson({{"schemaVersion", 900}}, &p, &error));
        QCOMPARE(p.name, QStringLiteral("保留原工程"));
        QVERIFY(!error.isEmpty());
    }
    void rejectsMalformedParametersAndUnknownBindings() {
        auto p = defaultProject();
        p.panels = {{"invalid-panel", "trend", "无效绑定", "untrusted-field"}};
        QVERIFY(!validateProject(p).isEmpty());
        p = defaultProject();
        p.flow.append({"delay-test", "delay", "等待", true, {{"ms", -5}}});
        QVERIFY(!validateProject(p).isEmpty());
    }
    void rejectsWrongDeviceRoutingTypeBeforeExecution() {
        auto p = defaultProject();
        p.flow.last().parameters = {{"deviceId", 123}};
        QVERIFY(!validateProject(p).isEmpty());
    }
    void rejectsInvalidMotionAndLightCommands_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QJsonObject>("parameters");
        QTest::newRow("axis-negative") << QString("move") << QJsonObject{{"axis", -1}};
        QTest::newRow("axis-fraction") << QString("move") << QJsonObject{{"axis", 1.5}};
        QTest::newRow("axis-string") << QString("move") << QJsonObject{{"axis", "1"}};
        QTest::newRow("speed-zero") << QString("move") << QJsonObject{{"speed", 0}};
        QTest::newRow("speed-string") << QString("move") << QJsonObject{{"speed", "fast"}};
        QTest::newRow("channel-negative") << QString("light") << QJsonObject{{"channel", -1}};
        QTest::newRow("channel-fraction") << QString("light") << QJsonObject{{"channel", 0.5}};
        QTest::newRow("enabled-string") << QString("light") << QJsonObject{{"enabled", "yes"}};
        QTest::newRow("brightness-fraction") << QString("light") << QJsonObject{{"brightness", 10.5}};
    }
    void rejectsInvalidMotionAndLightCommands() {
        QFETCH(QString, type);
        QFETCH(QJsonObject, parameters);
        auto project = defaultProject();
        for (auto &node : project.flow)
            if (node.type == type)
                node.parameters = parameters;
        QVERIFY2(!validateProject(project).isEmpty(), qPrintable(QString("accepted invalid %1").arg(type)));
    }
    void acceptsRobotCartesianGripAndCommittedSortWorkflow() {
        auto p = defaultProject();
        p.devices.append({"scara-test", "robot", "SCARA", "simulated"});
        p.flow.insert(3, {"robot-pick",
                          "robot_move",
                          "机械臂定位",
                          true,
                          {{"deviceId", "scara-test"}, {"x", 220}, {"y", 40}, {"z", 100}, {"r", 0}}});
        p.flow.insert(4, {"robot-grip", "robot_grip", "机械臂夹爪", true, {{"closed", true}}});
        p.flow.append({"robot-sort", "robot_sort", "视觉分拣", true, {{"restock", true}}});
        QVERIFY2(validateProject(p).isEmpty(), qPrintable(validateProject(p).join(";")));
        QCOMPARE(defaultProject().devices.last().category, QStringLiteral("robot"));
        QCOMPARE(defaultProject().devices.last().id, QStringLiteral("robot-1"));
        QCOMPARE(defaultProject().devices.at(1).id, QStringLiteral("motion-1"));
        QCOMPARE(defaultProject().devices.at(3).id, QStringLiteral("plc-1"));
        ProjectConfig loaded;
        QString error;
        QVERIFY2(projectFromJson(projectToJson(p), &loaded, &error), qPrintable(error));
        QCOMPARE(loaded.flow.last().type, QStringLiteral("robot_sort"));
    }

    void rejectsInvalidRobotCommandsBeforeExecution_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QJsonObject>("parameters");
        QTest::newRow("unreachable") << QString("robot_move") << QJsonObject{{"x", 400}};
        QTest::newRow("x-string") << QString("robot_move") << QJsonObject{{"x", "220"}};
        QTest::newRow("z-limit") << QString("robot_move") << QJsonObject{{"z", 151}};
        QTest::newRow("elbow-number") << QString("robot_move") << QJsonObject{{"elbowPositive", 1}};
        QTest::newRow("speed-zero") << QString("robot_move") << QJsonObject{{"speedPercent", 0}};
        QTest::newRow("speed-high") << QString("robot_sort") << QJsonObject{{"speedPercent", 101}};
        QTest::newRow("speed-string") << QString("robot_sort") << QJsonObject{{"speedPercent", "60"}};
        QTest::newRow("timeout-string") << QString("robot_move") << QJsonObject{{"timeoutMs", "5000"}};
        QTest::newRow("closed-string") << QString("robot_grip") << QJsonObject{{"closed", "true"}};
        QTest::newRow("restock-number") << QString("robot_sort") << QJsonObject{{"restock", 1}};
        QTest::newRow("outcome-override") << QString("robot_sort") << QJsonObject{{"outcome", "OK"}};
        QTest::newRow("wrong-category") << QString("robot_move") << QJsonObject{{"deviceId", "motion-1"}};
    }

    void rejectsInvalidRobotCommandsBeforeExecution() {
        QFETCH(QString, type);
        QFETCH(QJsonObject, parameters);
        auto p = defaultProject();
        p.devices.append({"scara-test", "robot", "SCARA", "simulated"});
        p.flow.append({"robot-command", type, "机械臂", true, parameters});
        QVERIFY(!validateProject(p).isEmpty());
    }

    void robotSortRequiresStoreAndSimulationBackend() {
        auto p = defaultProject();
        p.devices.append({"scara-test", "robot", "SCARA", "simulated"});
        p.flow.insert(0, {"robot-sort", "robot_sort", "视觉分拣", true, {}});
        QVERIFY(!validateProject(p).isEmpty());
        p.flow.removeFirst();
        p.flow.append({"robot-sort", "robot_sort", "视觉分拣", true, {}});
        p.flow.append({"robot-sort2", "robot_sort", "视觉分拣", true, {}});
        QVERIFY(!validateProject(p).isEmpty());
        p.flow.removeLast();
        p.devices.last().backend = "modbus-tcp";
        QVERIFY(!validateProject(p).isEmpty());
    }

    void rejectsMalformedIntegerJsonRatherThanDefaulting() {
        auto json = projectToJson(defaultProject());
        auto devices = json["devices"].toArray();
        auto device = devices[0].toObject();
        device["port"] = "bad";
        devices[0] = device;
        json["devices"] = devices;
        ProjectConfig p;
        QString error;
        QVERIFY(!projectFromJson(json, &p, &error));
        json = projectToJson(defaultProject());
        json["imageRetentionDays"] = "bad";
        QVERIFY(!projectFromJson(json, &p, &error));
        json = projectToJson(defaultProject());
        json["imageQuotaMB"] = 1e300;
        QVERIFY(!projectFromJson(json, &p, &error));
        json = projectToJson(defaultProject());
        devices = json["devices"].toArray();
        device = devices[0].toObject();
        device["unitId"] = 1.5;
        devices[0] = device;
        json["devices"] = devices;
        QVERIFY(!projectFromJson(json, &p, &error));
    }
    void measuresImageRatherThanSyntheticMetadata() {
        QImage image(640, 480, QImage::Format_RGB32);
        image.fill(QColor(20, 20, 20));
        QPainter painter(&image);
        painter.fillRect(QRect(220, 140, 200, 200), Qt::white);
        painter.end();
        ImageMeasurement measurement;
        QString error;
        QVERIFY2(analyzeFrame(image, 20.0, &measurement, &error), qPrintable(error));
        QCOMPARE(measurement.bounds, QRect(220, 140, 200, 200));
        QCOMPARE(measurement.diameterMm, 10.0);
        QCOMPARE(measurement.offsetXmm, 0.0);
        QCOMPARE(measurement.offsetYmm, 0.0);
    }
    void rejectsEmptyImageAndInvalidCalibration() {
        ImageMeasurement m;
        QString error;
        QVERIFY(!analyzeFrame({}, 20, &m, &error));
        QVERIFY(!error.isEmpty());
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::black);
        QVERIFY(!analyzeFrame(image, 0, &m, &error));
        QVERIFY(!analyzeFrame(image, 20, &m, &error));
    }
};
QTEST_GUILESS_MAIN(DomainTest)
#include "tst_domain.moc"
