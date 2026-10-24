#pragma once

#include "domain/RobotKinematics.h"
#include "domain/Types.h"
#include <QVector>
#include <functional>
#include <memory>

namespace ivs {

struct SimAxisState {
    double position = 0;
    double target = 0;
    double speed = 25;
    double minimum = -100;
    double maximum = 100;
    bool enabled = true;
    bool moving = false;
    bool homed = true;
    QString fault;
};

struct SimLightState {
    bool enabled = true;
    int brightness = 80; // 0..255; level80 preserves the existing demonstration exposure
};

struct SimRobotState {
    RobotJoints joints;
    RobotJoints target;
    RobotPose tcp;
    double speedPercent = 60;
    bool enabled = true;
    bool moving = false;
    bool gripperClosed = false;
    bool carryingPart = false;
    QString fault;
    QString partLocation = QStringLiteral("feed"); // feed/gripper/ok/ng/loose
    RobotPose partPose{220, 40, 20, 0};
    qint64 okCount = 0;
    qint64 ngCount = 0;
    bool programRunning = false;
    QString programName;
    int programStep = 0;
    int programSteps = 0;
    quint64 commandSequence = 0;
    quint64 completedSequence = 0;
    quint64 interruptedSequence = 0;
    QVector<RobotPose> trail;
    QVector<RobotWaypoint> taughtPoints;
};

struct SimDeviceState {
    QString id;
    QString category;
    QString model;
    QJsonObject parameters;
    bool connected = true;
    QVector<SimAxisState> axes;
    QVector<SimLightState> lights;
    QVector<quint16> holdingRegisters;
    QVector<quint16> inputRegisters;
    QVector<bool> coils;
    QVector<bool> discreteInputs;
    int pendingTriggers = 0;
    SimRobotState robot;
};

struct SimulationSnapshot {
    QList<SimDeviceState> devices;
    bool automaticTrigger = true;
    bool manualOverride = false;
    bool emergencyStop = false;
    QString partMode = QStringLiteral("alternating"); // alternating / good / ng / missing
    double pixelsPerMm = 20;
    quint64 revision = 0;
    // Never cleared by reset; active runs must be restarted after an interruption.
    quint64 interruptionGeneration = 0;
};

// Shared by the console and inspection worker. All methods are mutex protected;
// motion advances from a monotonic clock when state is read, not from a GUI timer.
class SimulationPlant final {
  public:
    using Clock = std::function<qint64()>; // monotonic milliseconds
    explicit SimulationPlant(Clock clock = {});
    ~SimulationPlant();
    SimulationPlant(const SimulationPlant &) = delete;
    SimulationPlant &operator=(const SimulationPlant &) = delete;

    bool configure(const QList<DeviceConfig> &devices, QString *error = nullptr);
    SimulationSnapshot snapshot() const;
    bool reset(QString *error = nullptr);
    bool setConnected(const QString &id, bool connected, QString *error = nullptr);
    bool enableAxis(const QString &id, int axis, bool enabled, QString *error = nullptr);
    bool moveAxis(const QString &id, int axis, double target, double speed, QString *error = nullptr);
    bool setAxisPosition(const QString &id, int axis, double position, QString *error = nullptr);
    bool homeAxis(const QString &id, int axis, double speed, QString *error = nullptr);
    bool stopAxis(const QString &id, int axis, QString *error = nullptr);
    void stopAllAxes();
    bool enableRobot(const QString &id, bool enabled, QString *error = nullptr);
    bool moveRobotJoints(const QString &id, const RobotJoints &target, double speedPercent,
                         QString *error = nullptr);
    bool moveRobotTcp(const QString &id, const RobotPose &target, double speedPercent,
                      bool elbowPositive = true, QString *error = nullptr);
    bool homeRobot(const QString &id, double speedPercent = 60, QString *error = nullptr);
    bool stopRobot(const QString &id, QString *error = nullptr);
    bool setRobotGripper(const QString &id, bool closed, QString *error = nullptr);
    bool restockRobot(const QString &id, QString *error = nullptr);
    bool startRobotSort(const QString &id, const QString &outcome, double speedPercent = 60,
                        QString *error = nullptr);
    bool teachRobotPoint(const QString &id, const QString &name, QString *error = nullptr);
    bool removeRobotPoint(const QString &id, int index, QString *error = nullptr);
    bool setRobotTeaching(const QString &id, const QVector<RobotWaypoint> &points, QString *error = nullptr);
    bool playRobotTeaching(const QString &id, double speedPercent = 60, QString *error = nullptr);
    bool setLight(const QString &id, int channel, bool enabled, int brightness, QString *error = nullptr);
    bool writeSignal(const QString &id, const QString &table, int address, quint16 value,
                     QString *error = nullptr);
    bool readSignal(const QString &id, const QString &table, int address, quint16 *value,
                    QString *error = nullptr) const;
    bool queueTrigger(const QString &id, QString *error = nullptr);
    bool takeTrigger(const QString &id, bool *ready, QString *error = nullptr);
    void setAutomaticTrigger(bool enabled);
    void setManualOverride(bool enabled);
    void setEmergencyStop(bool enabled);
    bool setPartMode(const QString &mode, QString *error = nullptr);
    bool setPixelsPerMm(double value, QString *error = nullptr);
    QImage renderFrame(const QString &cameraId, const QSize &size, quint64 sequence,
                       QString *error = nullptr) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ivs
