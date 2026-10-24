#include "infrastructure/Devices.h"
#include "infrastructure/ModbusTcpClient.h"
#include "infrastructure/SimulationPlant.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QSemaphore>
#include <QTcpServer>
#include <QTest>
#include <functional>
#include <thread>

using namespace ivs;

namespace {

// A real loopback TCP peer keeps the blocking client on its owning thread.
class TestServer {
  public:
    using Reply = std::function<QByteArray(const QByteArray &)>;
    explicit TestServer(QList<Reply> replies, bool split = false)
        : worker([this, replies, split] {
              QTcpServer server;
              if (server.listen(QHostAddress::LocalHost, 0))
                  port = server.serverPort();
              else
                  listenError = server.errorString();
              ready.release();
              if (!port || !server.waitForNewConnection(2500))
                  return;
              auto *socket = server.nextPendingConnection();
              for (const auto &reply : replies) {
                  QByteArray request;
                  while (request.size() < 12) {
                      if (!socket->bytesAvailable() && !socket->waitForReadyRead(1000))
                          return;
                      request += socket->readAll();
                  }
                  requests.append(request);
                  const QByteArray response = reply(request);
                  if (split && response.size() > 7) {
                      socket->write(response.left(4));
                      socket->waitForBytesWritten(1000);
                      std::this_thread::sleep_for(std::chrono::milliseconds(5));
                      socket->write(response.mid(4));
                  } else {
                      socket->write(response);
                  }
                  socket->waitForBytesWritten(1000);
              }
              // Deliver the final response before closing the connection.
              socket->disconnectFromHost();
          }) {
        ready.acquire();
    }
    ~TestServer() {
        if (worker.joinable())
            worker.join();
    }
    void join() {
        if (worker.joinable())
            worker.join();
    }
    quint16 port = 0;
    QString listenError;
    QList<QByteArray> requests;

  private:
    QSemaphore ready;
    std::thread worker;
};

QByteArray frame(const QByteArray &request, const QByteArray &pdu) {
    QByteArray response = request.left(4);
    response.append(char(0));
    response.append(char(pdu.size() + 1));
    response.append(request.at(6));
    return response + pdu;
}

DeviceConfig device(const QString &id, const QString &category,
                    const QString &backend = QStringLiteral("simulated")) {
    DeviceConfig config;
    config.id = id;
    config.category = category;
    config.backend = backend;
    config.model = QStringLiteral("test");
    return config;
}

QRect brightBounds(const QImage &image) {
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qGray(image.pixel(x, y)) > 128) {
                left = qMin(left, x);
                top = qMin(top, y);
                right = qMax(right, x);
                bottom = qMax(bottom, y);
            }
        }
    }
    return QRect(QPoint(left, top), QPoint(right, bottom));
}

} // namespace

class DeviceTests : public QObject {
    Q_OBJECT
  private slots:
    void simulationProducesMeasurableGeometry() {
        SimulatedCamera camera;
        QString error;
        const auto normal = camera.capture(QSize(640, 640), 1, &error);
        QVERIFY2(!normal.isNull(), qPrintable(error));
        QCOMPARE(brightBounds(normal).width(), 400);
        QCOMPARE(brightBounds(normal).height(), 400);
        QCOMPARE(brightBounds(camera.capture(QSize(2448, 2048), 5)).width(), 460);
        QCOMPARE(camera.capture(QSize(640, 640), 1), normal);
        QVERIFY(camera.capture(QSize(), 1, &error).isNull());
        QVERIFY(!error.isEmpty());
    }

    void simulatedLightChangesCapturedPixels() {
        DeviceGateway gateway;
        QString error;
        QVERIFY(gateway.configure({device("cam", "camera"), device("lamp", "light")}, &error));
        QVERIFY(gateway.connectAll(&error));
        const auto illuminated = gateway.capture(QSize(640, 640), 1, &error);
        QVERIFY(!illuminated.isNull());
        FlowNode light;
        light.type = "light";
        light.parameters = {{"deviceId", "lamp"}, {"brightness", 0}};
        QVERIFY2(gateway.execute(light, &error), qPrintable(error));
        const auto dark = gateway.capture(QSize(640, 640), 1, &error);
        QVERIFY2(!dark.isNull(), qPrintable(error));
        QVERIFY(qGray(dark.pixel(320, 320)) < 128);
        QVERIFY(illuminated != dark);
    }

    void simulatedExplicitTriggerChecksActualSignal() {
        DeviceGateway gateway;
        QString error;
        QVERIFY(gateway.configure({device("plc", "plc")}, &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode trigger;
        trigger.type = "trigger";
        trigger.parameters = {{"deviceId", "plc"}, {"address", 7}, {"requireValue", 1}};
        QCOMPARE(gateway.triggerReady(trigger, &error), TriggerReadiness::Waiting);
        FlowNode output;
        output.type = "output";
        output.parameters = {{"deviceId", "plc"}, {"address", 7}, {"value", 1}};
        QVERIFY2(gateway.execute(output, &error), qPrintable(error));
        QCOMPARE(gateway.triggerReady(trigger, &error), TriggerReadiness::Ready);
    }

    void simulatedMotionUsesSharedStateAndWaitsForArrival() {
        qint64 now = 0;
        auto plant = std::make_shared<SimulationPlant>([&] { return now; });
        DeviceGateway gateway(plant);
        QString error;
        const QList<DeviceConfig> configs{device("cam", "camera"), device("axis", "motion"),
                                          device("lamp", "light"), device("plc", "plc")};
        QVERIFY(gateway.configure(configs, &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode move;
        move.type = "move";
        move.parameters = {{"deviceId", "axis"}, {"axis", 0}, {"position", 2}, {"speed", 10}};
        QVERIFY2(gateway.execute(move, &error), qPrintable(error));
        QCOMPARE(gateway.motionReady(move, &error), TriggerReadiness::Waiting);
        now = 100;
        QCOMPARE(plant->snapshot().devices.at(1).axes.at(0).position, 1.0);
        now = 200;
        QCOMPARE(gateway.motionReady(move, &error), TriggerReadiness::Ready);
        QCOMPARE(plant->snapshot().devices.at(1).axes.at(0).position, 2.0);
        QCOMPARE(brightBounds(gateway.capture(QSize(640, 640), 1)).left(), 156);
        QVERIFY(gateway.configure(configs, &error));
        QCOMPARE(plant->snapshot().devices.at(1).axes.at(0).position, 2.0);
    }

    void stoppedMotionMustHaveReachedTheRequestedDestination() {
        qint64 now = 0;
        auto plant = std::make_shared<SimulationPlant>([&] { return now; });
        DeviceGateway gateway(plant);
        QString error;
        QVERIFY(gateway.configure({device("axis", "motion")}, &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode move;
        move.type = "move";
        move.parameters = {{"position", 5}, {"speed", 1}};
        QVERIFY(gateway.execute(move, &error));
        now = 500;
        QVERIFY(plant->stopAxis("axis", 0, &error));
        QCOMPARE(gateway.motionReady(move, &error), TriggerReadiness::Failed);
        QVERIFY(!error.isEmpty());
        plant->setManualOverride(true);
        QCOMPARE(gateway.motionReady(move, &error), TriggerReadiness::Ready);
        plant->setManualOverride(false);
        QVERIFY(gateway.execute(move, &error));
        QVERIFY(plant->moveAxis("axis", 0, 1, 1, &error));
        QCOMPARE(gateway.motionReady(move, &error), TriggerReadiness::Failed);
    }

    void gatewayRobotMoveUsesSharedStateAndIgnoresAxisLightOverride() {
        qint64 now = 0;
        auto plant = std::make_shared<SimulationPlant>([&] { return now; });
        DeviceGateway gateway(plant);
        QString error;
        QVERIFY2(gateway.configure({device("scara", "robot")}, &error), qPrintable(error));
        QVERIFY(gateway.connectAll(&error));
        plant->setManualOverride(true);
        FlowNode move{"robot-node",
                      "robot_move",
                      "机械臂定位",
                      true,
                      {{"deviceId", "scara"}, {"x", 220}, {"y", 40}, {"z", 20}, {"r", 0}}};
        QVERIFY2(gateway.execute(move, &error), qPrintable(error));
        QVERIFY(plant->snapshot().devices.first().robot.moving);
        now = 100000;
        const auto robot = plant->snapshot().devices.first().robot;
        QVERIFY(!robot.moving);
        QVERIFY(std::abs(robot.tcp.x - 220) < 1e-6);
        QVERIFY(std::abs(robot.tcp.y - 40) < 1e-6);
        QCOMPARE(robot.tcp.z, 20.0);
    }

    void robotReadinessRequiresTheExactIssuedCommand() {
        qint64 now = 0;
        auto plant = std::make_shared<SimulationPlant>([&] { return now; });
        DeviceGateway gateway(plant);
        QString error;
        QVERIFY(gateway.configure({device("a", "robot"), device("b", "robot")}, &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode move{"robot-node",
                      "robot_move",
                      "机械臂定位",
                      true,
                      {{"deviceId", "a"}, {"x", 220}, {"y", 40}, {"z", 20}, {"r", 0}}};
        QCOMPARE(gateway.robotReady(move, &error), TriggerReadiness::Failed);
        QVERIFY(gateway.execute(move, &error));
        QCOMPARE(gateway.robotReady(move, &error), TriggerReadiness::Waiting);
        auto changedRoute = move;
        changedRoute.parameters.insert("deviceId", "b");
        QCOMPARE(gateway.robotReady(changedRoute, &error), TriggerReadiness::Failed);
        QVERIFY(plant->stopRobot("a", &error));
        QCOMPARE(gateway.robotReady(move, &error), TriggerReadiness::Failed);
        now = 100000;
        QCOMPARE(gateway.robotReady(move, &error), TriggerReadiness::Failed);
        QVERIFY(gateway.execute(move, &error));
        QVERIFY(plant->moveRobotTcp("a", {180, 140, 100, 0}, 60, true, &error));
        QCOMPARE(gateway.robotReady(move, &error), TriggerReadiness::Failed);
        QVERIFY(gateway.execute(move, &error));
        now = 200000;
        QCOMPARE(gateway.robotReady(move, &error), TriggerReadiness::Ready);
        FlowNode grip{"grip", "robot_grip", "夹取", true, {{"deviceId", "a"}, {"closed", true}}};
        QVERIFY(gateway.execute(grip, &error));
        QCOMPARE(gateway.robotReady(grip, &error), TriggerReadiness::Ready);
        QVERIFY(plant->snapshot().devices.first().robot.carryingPart);
        FlowNode sort{"sort", "robot_sort", "分拣", true, {{"deviceId", "b"}}};
        QVERIFY(!gateway.execute(sort, &error));
        QVERIFY(!gateway.executeRobotSort(sort, "ERROR", &error));
        sort.parameters.insert("outcome", "NG");
        QVERIFY(!gateway.executeRobotSort(sort, "OK", &error));
    }

    void robotGatewayRejectsInvalidCommands_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QJsonObject>("parameters");
        QTest::newRow("wrong-route") << QString("robot_move") << QJsonObject{{"deviceId", "axis"}};
        QTest::newRow("missing-route") << QString("robot_move") << QJsonObject{{"deviceId", "missing"}};
        QTest::newRow("numeric-route") << QString("robot_move") << QJsonObject{{"deviceId", 1}};
        QTest::newRow("x-string") << QString("robot_move") << QJsonObject{{"x", "220"}};
        QTest::newRow("unreachable") << QString("robot_move") << QJsonObject{{"x", 500}};
        QTest::newRow("speed-zero") << QString("robot_move") << QJsonObject{{"speedPercent", 0}};
        QTest::newRow("speed-high") << QString("robot_move") << QJsonObject{{"speedPercent", 101}};
        QTest::newRow("elbow-number") << QString("robot_move") << QJsonObject{{"elbowPositive", 1}};
        QTest::newRow("grip-number") << QString("robot_grip") << QJsonObject{{"closed", 1}};
        QTest::newRow("timeout-string") << QString("robot_move") << QJsonObject{{"timeoutMs", "5000"}};
        QTest::newRow("restock-string") << QString("robot_sort") << QJsonObject{{"restock", "yes"}};
        QTest::newRow("outcome-override") << QString("robot_sort") << QJsonObject{{"outcome", "NG"}};
    }

    void robotGatewayRejectsInvalidCommands() {
        QFETCH(QString, type);
        QFETCH(QJsonObject, parameters);
        DeviceGateway gateway;
        QString error;
        QVERIFY(gateway.configure({device("a", "robot"), device("axis", "motion")}, &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode node{"robot-node", type, "机械臂", true, parameters};
        const bool accepted = type == "robot_sort" ? gateway.executeRobotSort(node, "OK", &error)
                                                   : gateway.execute(node, &error);
        QVERIFY(!accepted);
        QVERIFY(!error.isEmpty());
        QVERIFY(!gateway.configure({device("real", "robot", "modbus-tcp")}, &error));
    }

    void manualTriggerCreditsAreConsumedAndSignalsRemainIndependent() {
        auto plant = std::make_shared<SimulationPlant>();
        DeviceGateway gateway(plant);
        QString error;
        QVERIFY(gateway.configure({device("plc", "plc"), device("io", "io")}, &error));
        QVERIFY(gateway.connectAll(&error));
        plant->setAutomaticTrigger(false);
        FlowNode trigger;
        trigger.type = "trigger";
        trigger.parameters = {{"deviceId", "plc"}};
        QCOMPARE(gateway.triggerReady(trigger, &error), TriggerReadiness::Waiting);
        QVERIFY(plant->queueTrigger("plc", &error));
        QCOMPARE(gateway.triggerReady(trigger, &error), TriggerReadiness::Ready);
        QCOMPARE(gateway.triggerReady(trigger, &error), TriggerReadiness::Waiting);
        QVERIFY(plant->writeSignal("plc", "discrete", 0, 1, &error));
        QCOMPARE(gateway.triggerReady(trigger, &error), TriggerReadiness::Ready);
        trigger.parameters.insert("deviceId", "io");
        QCOMPARE(gateway.triggerReady(trigger, &error), TriggerReadiness::Waiting);
        FlowNode output;
        output.type = "output";
        output.parameters = {{"deviceId", "io"}, {"function", "coil"}, {"address", 3}, {"value", 1}};
        QVERIFY(gateway.execute(output, &error));
        quint16 value = 0;
        QVERIFY(plant->readSignal("io", "coil", 3, &value, &error));
        QCOMPARE(value, quint16(1));
        QVERIFY(plant->readSignal("plc", "coil", 3, &value, &error));
        QCOMPARE(value, quint16(0));
    }

    void manualOverrideRetainsConsoleMotionAndLighting() {
        auto plant = std::make_shared<SimulationPlant>();
        DeviceGateway gateway(plant);
        QString error;
        QVERIFY(gateway.configure({device("axis", "motion"), device("lamp", "light")}, &error));
        QVERIFY(gateway.connectAll(&error));
        QVERIFY(plant->setAxisPosition("axis", 0, 7, &error));
        QVERIFY(plant->setLight("lamp", 0, false, 3, &error));
        plant->setManualOverride(true);
        FlowNode move;
        move.type = "move";
        move.parameters = {{"position", 0}};
        QVERIFY(gateway.execute(move, &error));
        FlowNode light;
        light.type = "light";
        light.parameters = {{"brightness", 80}};
        QVERIFY(gateway.execute(light, &error));
        const auto state = plant->snapshot();
        QCOMPARE(state.devices.at(0).axes.at(0).position, 7.0);
        QVERIFY(!state.devices.at(1).lights.at(0).enabled);
        QCOMPARE(state.devices.at(1).lights.at(0).brightness, 3);
        QVERIFY(plant->enableAxis("axis", 0, false, &error));
        QCOMPARE(gateway.motionReady(move, &error), TriggerReadiness::Failed);
    }

    void simulationRejectsMalformedNodeValues_data() {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QJsonObject>("parameters");
        QTest::newRow("position-string") << QString("move") << QJsonObject{{"position", "2"}};
        QTest::newRow("speed-high") << QString("move") << QJsonObject{{"speed", 1001}};
        QTest::newRow("speed-zero") << QString("move") << QJsonObject{{"speed", 0}};
        QTest::newRow("axis-fraction") << QString("move") << QJsonObject{{"axis", 0.5}};
        QTest::newRow("soft-limit") << QString("move") << QJsonObject{{"position", 101}};
        QTest::newRow("timeout-string") << QString("move") << QJsonObject{{"timeoutMs", "1000"}};
        QTest::newRow("brightness-string") << QString("light") << QJsonObject{{"brightness", "80"}};
        QTest::newRow("brightness-high") << QString("light") << QJsonObject{{"brightness", 256}};
        QTest::newRow("enabled-number") << QString("light") << QJsonObject{{"enabled", 1}};
        QTest::newRow("output-address") << QString("output") << QJsonObject{{"address", 64}};
        QTest::newRow("output-coil-value")
            << QString("output") << QJsonObject{{"function", "coil"}, {"value", 2}};
        QTest::newRow("output-function") << QString("output") << QJsonObject{{"function", 1}};
    }

    void simulationRejectsMalformedNodeValues() {
        QFETCH(QString, type);
        QFETCH(QJsonObject, parameters);
        DeviceGateway gateway;
        QString error;
        QVERIFY(gateway.configure({device("axis", "motion"), device("lamp", "light"), device("plc", "plc")},
                                  &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode node;
        node.type = type;
        node.parameters = parameters;
        QVERIFY(!gateway.execute(node, &error));
        QVERIFY(!error.isEmpty());
    }

    void virtualEmergencyAndDisconnectDoNotAffectRealModbus() {
        TestServer peer({[](const QByteArray &r) { return r; }});
        QVERIFY2(peer.port, qPrintable(peer.listenError));
        auto plant = std::make_shared<SimulationPlant>();
        DeviceGateway gateway(plant);
        auto plc = device("plc", "plc", "modbus-tcp");
        plc.port = peer.port;
        QString error;
        QVERIFY(gateway.configure({plc, device("lamp", "light")}, &error));
        QVERIFY(gateway.connectAll(&error));
        plant->setEmergencyStop(true);
        FlowNode output;
        output.type = "output";
        output.parameters = {{"deviceId", "plc"}, {"address", 9}, {"value", 3}};
        QVERIFY2(gateway.execute(output, &error), qPrintable(error));
        FlowNode light;
        light.type = "light";
        QVERIFY(!gateway.execute(light, &error));
        plant->setEmergencyStop(false);
        QVERIFY(plant->setConnected("lamp", false, &error));
        QVERIFY(!gateway.execute(light, &error));
        QVERIFY(error.contains("disconnected"));
        peer.join();
        QCOMPARE(peer.requests.first().mid(7).toHex(), QByteArray("0600090003"));
    }

    void readsRegistersAndBitsUsingWireProtocol() {
        TestServer peer({[](const QByteArray &r) { return frame(r, QByteArray::fromHex("03041234abcd")); },
                         [](const QByteArray &r) { return frame(r, QByteArray::fromHex("0402002a")); },
                         [](const QByteArray &r) { return frame(r, QByteArray::fromHex("01024d01")); },
                         [](const QByteArray &r) { return frame(r, QByteArray::fromHex("020105")); }},
                        true);
        QVERIFY2(peer.port, qPrintable(peer.listenError));
        ModbusTcpClient client;
        QString error;
        QVERIFY2(client.connectToHost(QStringLiteral("127.0.0.1"), peer.port, 7, 500, &error),
                 qPrintable(error));
        QCOMPARE(client.readHoldingRegisters(10, 2, &error), (QList<quint16>{0x1234, 0xabcd}));
        QCOMPARE(client.readInputRegisters(20, 1, &error), (QList<quint16>{42}));
        QCOMPARE(client.readCoils(30, 9, &error),
                 (QList<bool>{true, false, true, true, false, false, true, false, true}));
        QCOMPARE(client.readDiscreteInputs(40, 3, &error), (QList<bool>{true, false, true}));
        QVERIFY(error.isEmpty());
        peer.join();
        QCOMPARE(peer.requests.size(), 4);
        QCOMPARE(peer.requests.at(0).mid(2).toHex(), QByteArray("000000060703000a0002"));
        QCOMPARE(peer.requests.at(1).mid(2).toHex(), QByteArray("00000006070400140001"));
        QVERIFY(peer.requests.at(0).left(2) != peer.requests.at(1).left(2));
    }

    void writesRequireExactEcho() {
        TestServer peer({[](const QByteArray &r) { return r; }, [](const QByteArray &r) { return r; },
                         [](const QByteArray &r) {
                             QByteArray bad = r;
                             bad[11] = 1;
                             return bad;
                         }});
        QVERIFY2(peer.port, qPrintable(peer.listenError));
        ModbusTcpClient client;
        QString error;
        QVERIFY(client.connectToHost(QStringLiteral("127.0.0.1"), peer.port, 1, 500, &error));
        QVERIFY(client.writeSingleRegister(100, 500, &error));
        QVERIFY(client.writeSingleCoil(15, true, &error));
        QVERIFY(!client.writeSingleRegister(101, 500, &error));
        QVERIFY(!error.isEmpty());
        peer.join();
        QCOMPARE(peer.requests.at(0).mid(7).toHex(), QByteArray("06006401f4"));
        QCOMPARE(peer.requests.at(1).mid(7).toHex(), QByteArray("05000fff00"));
    }

    void rejectsMalformedReplies_data() {
        QTest::addColumn<QByteArray>("pdu");
        QTest::addColumn<int>("corruptIndex");
        QTest::newRow("exception") << QByteArray::fromHex("8302") << -1;
        QTest::newRow("transaction") << QByteArray::fromHex("0302002a") << 1;
        QTest::newRow("protocol") << QByteArray::fromHex("0302002a") << 3;
        QTest::newRow("length") << QByteArray::fromHex("0302002a") << 4;
        QTest::newRow("unit") << QByteArray::fromHex("0302002a") << 6;
        QTest::newRow("function") << QByteArray::fromHex("0402002a") << -1;
        QTest::newRow("byte-count") << QByteArray::fromHex("0304002a") << -1;
    }

    void rejectsMalformedReplies() {
        QFETCH(QByteArray, pdu);
        QFETCH(int, corruptIndex);
        TestServer peer({[=](const QByteArray &r) {
            QByteArray response = frame(r, pdu);
            if (corruptIndex >= 0)
                response[corruptIndex] = char(response.at(corruptIndex) ^ 1);
            return response;
        }});
        QVERIFY2(peer.port, qPrintable(peer.listenError));
        ModbusTcpClient client;
        QString error;
        QVERIFY(client.connectToHost(QStringLiteral("127.0.0.1"), peer.port, 1, 100, &error));
        QVERIFY(client.readHoldingRegisters(0, 1, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        if (pdu.at(0) == char(0x83))
            QVERIFY(error.contains(QStringLiteral("exception"), Qt::CaseInsensitive));
    }

    void validatesConfigurationAndRequestBounds() {
        ModbusTcpClient client;
        QString error;
        QVERIFY(!client.connectToHost(QString(), 502, 1, 100, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!client.connectToHost(QStringLiteral("127.0.0.1"), 502, 256, 100, &error));
        QVERIFY(!client.connectToHost(QStringLiteral("127.0.0.1"), 65536, 1, 100, &error));
        QVERIFY(!client.connectToHost(QStringLiteral("127.0.0.1"), 502, 1, 0, &error));
        QVERIFY(client.readHoldingRegisters(65535, 2, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        QVERIFY(client.readHoldingRegisters(0, 0, &error).isEmpty());
        QVERIFY(client.readCoils(0, 2001, &error).isEmpty());
        QVERIFY(!client.writeSingleRegister(0, 1, &error));
    }

    void timesOutWithoutAcceptingLateResponse() {
        TestServer peer({[](const QByteArray &r) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            return frame(r, QByteArray::fromHex("03020001"));
        }});
        QVERIFY2(peer.port, qPrintable(peer.listenError));
        ModbusTcpClient client;
        QString error;
        QVERIFY(client.connectToHost(QStringLiteral("127.0.0.1"), peer.port, 1, 30, &error));
        QElapsedTimer elapsed;
        elapsed.start();
        QVERIFY(client.readHoldingRegisters(0, 1, &error).isEmpty());
        QVERIFY(error.contains(QStringLiteral("timeout"), Qt::CaseInsensitive));
        QVERIFY(elapsed.elapsed() < 500);
        QVERIFY(!client.writeSingleRegister(0, 1, &error));
        QVERIFY(error.contains(QStringLiteral("not connected"), Qt::CaseInsensitive));
    }

    void gatewaySimulatesButRejectsUnsupportedAdapters() {
        DeviceGateway gateway;
        QString error;
        const QList<DeviceConfig> configs{device("cam", "camera"), device("axis", "motion"),
                                          device("lamp", "light"), device("plc", "plc"), device("io", "io")};
        QVERIFY2(gateway.configure(configs, &error), qPrintable(error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode node;
        node.type = "motion";
        node.parameters = {{"deviceId", "axis"}};
        QVERIFY(gateway.execute(node, &error));
        QVERIFY(!gateway.capture(QSize(640, 640), 1, &error).isNull());
        gateway.disconnectAll();
        QVERIFY(gateway.capture(QSize(640, 640), 1, &error).isNull());
        QVERIFY(!error.isEmpty());
        QVERIFY(!gateway.configure({device("axis", "motion", "gts")}, &error));
        QVERIFY(error.contains("SDK", Qt::CaseInsensitive) ||
                error.contains("unsupported", Qt::CaseInsensitive));
        QVERIFY(!gateway.configure({device("cam", "camera", "mvs")}, &error));
        QVERIFY(error.contains("unavailable", Qt::CaseInsensitive));
        QVERIFY(!gateway.configure({device("a", "camera"), device("a", "camera")}, &error));
        QCOMPARE(hardwareCatalog().size() >= 8, true);
    }

    void gatewayReadsTriggerAndWritesOutput() {
        TestServer peer({[](const QByteArray &r) { return frame(r, QByteArray::fromHex("03020001")); },
                         [](const QByteArray &r) { return r; },
                         [](const QByteArray &r) { return frame(r, QByteArray::fromHex("03020000")); }});
        QVERIFY2(peer.port, qPrintable(peer.listenError));
        DeviceGateway gateway;
        QString error;
        auto plc = device("plc", "plc", "modbus-tcp");
        plc.port = peer.port;
        QVERIFY(gateway.configure({plc}, &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode trigger;
        trigger.type = "trigger";
        trigger.parameters = {{"deviceId", "plc"}, {"address", 10}, {"requireValue", 1}};
        QVERIFY2(gateway.execute(trigger, &error), qPrintable(error));
        FlowNode output;
        output.type = "output";
        output.parameters = {{"deviceId", "plc"}, {"address", 20}, {"value", 1}};
        QVERIFY2(gateway.execute(output, &error), qPrintable(error));
        QVERIFY(!gateway.execute(trigger, &error));
        QVERIFY(!error.isEmpty());
        gateway.disconnectAll();
        peer.join();
        QCOMPARE(peer.requests.at(1).mid(7).toHex(), QByteArray("0600140001"));
    }

    void gatewayRejectsNonStringDeviceIds_data() {
        QTest::addColumn<QString>("type");
        QTest::newRow("output") << QStringLiteral("output");
        QTest::newRow("trigger-execute") << QStringLiteral("trigger");
        QTest::newRow("trigger-readiness") << QStringLiteral("trigger-ready");
        QTest::newRow("motion") << QStringLiteral("move");
        QTest::newRow("light") << QStringLiteral("light");
    }

    void gatewayRejectsNonStringDeviceIds() {
        QFETCH(QString, type);
        DeviceGateway gateway;
        QString error;
        QVERIFY(gateway.configure({device("plc", "plc"), device("axis", "motion"), device("lamp", "light")},
                                  &error));
        QVERIFY(gateway.connectAll(&error));
        FlowNode node;
        node.type = type;
        node.parameters = {{"deviceId", 123}, {"address", 0}, {"value", 1}};
        if (type == QLatin1String("trigger-ready")) {
            node.type = QStringLiteral("trigger");
            QCOMPARE(gateway.triggerReady(node, &error), TriggerReadiness::Failed);
        } else
            QVERIFY(!gateway.execute(node, &error));
        QVERIFY(error.contains(QStringLiteral("deviceId")));
    }
};

QTEST_GUILESS_MAIN(DeviceTests)
#include "tst_devices.moc"
