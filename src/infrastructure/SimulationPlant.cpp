#include "SimulationPlant.h"

#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QStringList>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace ivs {
namespace {
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
void clearError(QString *error) {
    if (error)
        error->clear();
}
bool signalDevice(const QString &category) {
    return category == QLatin1String("plc") || category == QLatin1String("io");
}
SimDeviceState initialState(const QString &id, const QString &category, const QString &model,
                            const QJsonObject &parameters) {
    SimDeviceState state;
    state.id = id;
    state.category = category;
    state.model = model;
    state.parameters = parameters;
    if (category == QLatin1String("motion"))
        state.axes = QVector<SimAxisState>(4);
    else if (category == QLatin1String("robot")) {
        state.robot.tcp = robotForward(state.robot.joints);
        state.robot.target = state.robot.joints;
        state.robot.trail.append(state.robot.tcp);
    } else if (category == QLatin1String("light"))
        state.lights = QVector<SimLightState>(4);
    else if (signalDevice(category)) {
        state.holdingRegisters = QVector<quint16>(64, 0);
        state.inputRegisters = QVector<quint16>(64, 0);
        state.coils = QVector<bool>(16, false);
        state.discreteInputs = QVector<bool>(16, false);
    }
    return state;
}
int tableSize(const QString &table) {
    if (table == QLatin1String("holding") || table == QLatin1String("input"))
        return 64;
    if (table == QLatin1String("coil") || table == QLatin1String("discrete"))
        return 16;
    return 0;
}
bool validateSignal(const QString &table, int address, QString *error) {
    const int size = tableSize(table);
    if (!size)
        return fail(
            error,
            QStringLiteral("Unknown signal table '%1'; use holding, input, coil or discrete").arg(table));
    if (address < 0 || address >= size)
        return fail(error, QStringLiteral("%1 address must be in 0..%2").arg(table).arg(size - 1));
    return true;
}
} // namespace

struct SimulationPlant::Impl {
    mutable QMutex mutex;
    Clock clock;
    qint64 lastTime;
    SimulationSnapshot state;
    QSet<QString> homingAxes;
    struct RobotStep {
        RobotJoints target;
        bool motion = true;
        bool applyGripper = false;
        bool gripperClosed = false;
    };
    struct RobotExecution {
        RobotJoints start;
        double duration = 0;
        double elapsed = 0;
        quint64 sequence = 0;
        QVector<RobotStep> steps;
        int next = 0;
        bool pendingGripper = false;
        bool gripperClosed = false;
    };
    QHash<QString, RobotExecution> robots;

    explicit Impl(Clock suppliedClock) : clock(std::move(suppliedClock)) {
        if (!clock)
            clock = [] {
                return std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                    .count();
            };
        lastTime = clock();
    }
    QString axisKey(const QString &id, int axis) const {
        return id + QChar(0) + QString::number(axis);
    }
    void advance() {
        const qint64 current = clock();
        if (current <= lastTime)
            return;
        const double elapsed = (double(current) - double(lastTime)) / 1000.0;
        lastTime = current;
        bool changed = false;
        for (auto &device : state.devices) {
            if (device.category == QLatin1String("robot"))
                changed = advanceRobot(device, elapsed) || changed;
            for (int index = 0; index < device.axes.size(); ++index) {
                auto &axis = device.axes[index];
                if (!axis.moving)
                    continue;
                const double difference = axis.target - axis.position;
                const double travel = axis.speed * elapsed;
                if (travel >= std::abs(difference)) {
                    axis.position = axis.target;
                    axis.moving = false;
                    if (homingAxes.remove(axisKey(device.id, index)))
                        axis.homed = true;
                } else
                    axis.position += std::copysign(travel, difference);
                changed = true;
            }
        }
        if (changed)
            ++state.revision;
    }
    SimDeviceState *find(const QString &id, const QStringList &categories, QString *error,
                         bool requireConnection = true) {
        for (auto &device : state.devices) {
            if (device.id != id || id.isEmpty())
                continue;
            if (!categories.isEmpty() && !categories.contains(device.category)) {
                fail(error, QStringLiteral("Device '%1' has category '%2'; requires %3")
                                .arg(id, device.category, categories.join('/')));
                return nullptr;
            }
            if (requireConnection && !device.connected) {
                fail(error, QStringLiteral("Simulated device '%1' is disconnected").arg(id));
                return nullptr;
            }
            return &device;
        }
        fail(error, QStringLiteral("Simulated device '%1' does not exist").arg(id));
        return nullptr;
    }
    SimAxisState *axis(const QString &id, int index, QString *error) {
        auto *device = find(id, {QStringLiteral("motion")}, error);
        if (!device)
            return nullptr;
        if (index < 0 || index >= device->axes.size()) {
            fail(error, QStringLiteral("Axis must be in 0..3"));
            return nullptr;
        }
        return &device->axes[index];
    }
    void cancel(const QString &id, int index, SimAxisState &axis) {
        axis.moving = false;
        axis.target = axis.position;
        homingAxes.remove(axisKey(id, index));
    }
    SimDeviceState *robot(const QString &id, QString *error, bool requireConnection = true) {
        return find(id, {QStringLiteral("robot")}, error, requireConnection);
    }
    bool robotReady(const SimDeviceState &device, QString *error, bool allowProgram = false) const {
        if (state.emergencyStop)
            return fail(error, QStringLiteral("Simulation emergency stop is active"));
        if (!device.robot.enabled)
            return fail(error, QStringLiteral("Robot '%1' is disabled").arg(device.id));
        if (!allowProgram && device.robot.programRunning)
            return fail(error,
                        QStringLiteral("Robot '%1' is running a program; stop it first").arg(device.id));
        return true;
    }
    bool validSpeed(double speed, QString *error) const {
        if (!std::isfinite(speed) || speed < 1 || speed > 100)
            return fail(error, QStringLiteral("Robot speed must be finite and in 1..100 percent"));
        return true;
    }
    void updateRobotPose(SimRobotState &robot) {
        robot.tcp = robotForward(robot.joints);
        if (robot.carryingPart)
            robot.partPose = robot.tcp;
        if (robot.trail.isEmpty() || std::abs(robot.trail.last().x - robot.tcp.x) > 1e-9 ||
            std::abs(robot.trail.last().y - robot.tcp.y) > 1e-9 ||
            std::abs(robot.trail.last().z - robot.tcp.z) > 1e-9 ||
            std::abs(robot.trail.last().r - robot.tcp.r) > 1e-9) {
            robot.trail.append(robot.tcp);
            if (robot.trail.size() > 240)
                robot.trail.remove(0, robot.trail.size() - 240);
        }
    }
    bool nearPart(const RobotPose &a, const RobotPose &b) const {
        return std::hypot(std::hypot(a.x - b.x, a.y - b.y), a.z - b.z) <= 15;
    }
    void grip(SimRobotState &robot, bool closed) {
        robot.gripperClosed = closed;
        if (closed && !robot.carryingPart && robot.partLocation == QLatin1String("feed") &&
            nearPart(robot.tcp, robot.partPose)) {
            robot.carryingPart = true;
            robot.partLocation = QStringLiteral("gripper");
            robot.partPose = robot.tcp;
        } else if (!closed && robot.carryingPart) {
            robot.carryingPart = false;
            robot.partPose = robot.tcp;
            if (nearPart(robot.tcp, {80, 200, 20, 0})) {
                robot.partLocation = QStringLiteral("ok");
                ++robot.okCount;
            } else if (nearPart(robot.tcp, {80, -200, 20, 0})) {
                robot.partLocation = QStringLiteral("ng");
                ++robot.ngCount;
            } else
                robot.partLocation = QStringLiteral("loose");
        }
    }
    void cancelRobot(SimDeviceState &device) {
        auto &robot = device.robot;
        if (robot.moving || robot.programRunning) {
            const auto it = robots.constFind(device.id);
            const auto sequence = it == robots.cend() ? robot.commandSequence : it->sequence;
            robot.interruptedSequence = std::max(robot.interruptedSequence, sequence);
        }
        robot.moving = false;
        robot.target = robot.joints;
        robot.programRunning = false;
        robot.programName.clear();
        robot.programStep = 0;
        robot.programSteps = 0;
        robots.remove(device.id);
    }
    void segment(SimRobotState &robot, RobotExecution &execution, const RobotJoints &target) {
        execution.start = robot.joints;
        execution.elapsed = 0;
        const double scale = robot.speedPercent / 100;
        execution.duration = std::max({std::abs(target.j1 - robot.joints.j1) / (90 * scale),
                                       std::abs(target.j2 - robot.joints.j2) / (90 * scale),
                                       std::abs(target.z - robot.joints.z) / (150 * scale),
                                       std::abs(target.j4 - robot.joints.j4) / (90 * scale)});
        robot.target = target;
        robot.moving = execution.duration > 0;
    }
    bool advanceRobot(SimDeviceState &device, double seconds) {
        auto &robot = device.robot;
        if (!robot.moving && !robot.programRunning)
            return false;
        auto &execution = robots[device.id];
        for (;;) {
            if (robot.moving) {
                const double remaining = std::max(0.0, execution.duration - execution.elapsed);
                const double used = std::min(seconds, remaining);
                seconds -= used;
                execution.elapsed += used;
                if (execution.elapsed >= execution.duration) {
                    robot.joints = robot.target;
                    robot.moving = false;
                } else {
                    const double ratio = execution.elapsed / execution.duration;
                    robot.joints = {execution.start.j1 + (robot.target.j1 - execution.start.j1) * ratio,
                                    execution.start.j2 + (robot.target.j2 - execution.start.j2) * ratio,
                                    execution.start.z + (robot.target.z - execution.start.z) * ratio,
                                    execution.start.j4 + (robot.target.j4 - execution.start.j4) * ratio};
                }
                updateRobotPose(robot);
                if (robot.moving)
                    return true;
                if (execution.pendingGripper) {
                    grip(robot, execution.gripperClosed);
                    execution.pendingGripper = false;
                }
            }
            if (robot.programRunning && execution.next < execution.steps.size()) {
                const auto step = execution.steps[execution.next++];
                robot.programStep = execution.next;
                if (step.motion) {
                    execution.pendingGripper = step.applyGripper;
                    execution.gripperClosed = step.gripperClosed;
                    segment(robot, execution, step.target);
                    if (!robot.moving && execution.pendingGripper) {
                        grip(robot, execution.gripperClosed);
                        execution.pendingGripper = false;
                    }
                } else if (step.applyGripper)
                    grip(robot, step.gripperClosed);
                continue;
            }
            robot.programRunning = false;
            robot.programName.clear();
            robot.completedSequence = std::max(robot.completedSequence, execution.sequence);
            execution.steps.clear();
            return true;
        }
    }
    bool startRobotMove(SimDeviceState &device, const RobotJoints &target, double speed, QString *error) {
        if (!robotReady(device, error) || !validateRobotJoints(target, error) || !validSpeed(speed, error))
            return false;
        cancelRobot(device);
        auto &robot = device.robot;
        robot.speedPercent = speed;
        robot.fault.clear();
        auto &execution = robots[device.id];
        execution.sequence = ++robot.commandSequence;
        segment(robot, execution, target);
        if (!robot.moving)
            robot.completedSequence = execution.sequence;
        ++state.revision;
        return true;
    }
    void startRobotProgram(SimDeviceState &device, const QString &name, QVector<RobotStep> steps,
                           double speed) {
        auto &robot = device.robot;
        robot.speedPercent = speed;
        robot.fault.clear();
        robot.programName = name;
        robot.programRunning = true;
        robot.programStep = 0;
        robot.programSteps = steps.size();
        RobotExecution execution;
        execution.sequence = ++robot.commandSequence;
        execution.steps = std::move(steps);
        robots.insert(device.id, std::move(execution));
        advanceRobot(device, 0);
        ++state.revision;
    }
    bool teachingEditable(const SimDeviceState &device, QString *error) const {
        if (device.robot.moving || device.robot.programRunning)
            return fail(error, QStringLiteral("Stop the robot before editing teaching points"));
        return true;
    }
    bool validateTeaching(const QVector<RobotWaypoint> &points, QString *error) const {
        if (points.size() > 100)
            return fail(error, QStringLiteral("Robot teaching is limited to 100 points"));
        for (const auto &point : points) {
            if (point.name.trimmed().isEmpty())
                return fail(error, QStringLiteral("Robot teaching point name must not be empty"));
            if (!validateRobotJoints(point.joints, error))
                return false;
        }
        return true;
    }
    bool startMove(const QString &id, int index, double target, double speed, bool home, QString *error) {
        auto *value = axis(id, index, error);
        if (!value)
            return false;
        if (state.emergencyStop)
            return fail(error, QStringLiteral("Simulation emergency stop is active"));
        if (!value->enabled)
            return fail(error, QStringLiteral("Axis %1 on '%2' is disabled").arg(index).arg(id));
        if (!std::isfinite(target) || target < value->minimum || target > value->maximum)
            return fail(error, QStringLiteral("Axis target must be within soft limits %1..%2 mm")
                                   .arg(value->minimum)
                                   .arg(value->maximum));
        if (!std::isfinite(speed) || speed <= 0 || speed > 1000)
            return fail(error, QStringLiteral("Axis speed must be finite and in (0, 1000] mm/s"));
        value->target = target;
        value->speed = speed;
        value->moving = value->position != target;
        value->fault.clear();
        homingAxes.remove(axisKey(id, index));
        if (home) {
            value->homed = !value->moving;
            if (value->moving)
                homingAxes.insert(axisKey(id, index));
        }
        ++state.revision;
        return true;
    }
};

SimulationPlant::SimulationPlant(Clock clock) : d(std::make_unique<Impl>(std::move(clock))) {}
SimulationPlant::~SimulationPlant() = default;

bool SimulationPlant::configure(const QList<DeviceConfig> &devices, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    QSet<QString> ids;
    const QStringList categories{QStringLiteral("camera"), QStringLiteral("motion"), QStringLiteral("light"),
                                 QStringLiteral("plc"),    QStringLiteral("io"),     QStringLiteral("robot")};
    for (const auto &device : devices) {
        if (device.id.trimmed().isEmpty() || ids.contains(device.id))
            return fail(error, QStringLiteral("Device IDs must be nonempty and unique: '%1'").arg(device.id));
        if (!categories.contains(device.category))
            return fail(error, QStringLiteral("Unsupported device category '%1'").arg(device.category));
        ids.insert(device.id);
    }
    d->advance();
    QList<SimDeviceState> configured;
    QSet<QString> retainedHoming;
    QSet<QString> retainedRobots;
    for (const auto &device : devices) {
        if (device.backend.trimmed().toLower() != QLatin1String("simulated"))
            continue;
        auto next = initialState(device.id, device.category, device.model, device.parameters);
        for (const auto &old : d->state.devices)
            if (old.id == device.id && old.category == device.category) {
                next = old;
                next.model = device.model;
                next.parameters = device.parameters;
                if (next.category == QLatin1String("robot"))
                    retainedRobots.insert(next.id);
                for (int axis = 0; axis < next.axes.size(); ++axis) {
                    const auto key = d->axisKey(next.id, axis);
                    if (d->homingAxes.contains(key))
                        retainedHoming.insert(key);
                }
                break;
            }
        configured.append(std::move(next));
    }
    d->state.devices = std::move(configured);
    d->homingAxes = std::move(retainedHoming);
    for (auto it = d->robots.begin(); it != d->robots.end();)
        if (!retainedRobots.contains(it.key()))
            it = d->robots.erase(it);
        else
            ++it;
    ++d->state.revision;
    return true;
}

SimulationSnapshot SimulationPlant::snapshot() const {
    QMutexLocker lock(&d->mutex);
    d->advance();
    return d->state;
}

bool SimulationPlant::reset(QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    const auto devices = d->state.devices;
    const quint64 revision = d->state.revision;
    const quint64 interruptionGeneration = d->state.interruptionGeneration;
    d->state = SimulationSnapshot{};
    for (const auto &device : devices) {
        auto next = initialState(device.id, device.category, device.model, device.parameters);
        if (device.category == QLatin1String("robot"))
            next.robot.taughtPoints = device.robot.taughtPoints;
        d->state.devices.append(std::move(next));
    }
    d->state.revision = revision + 1;
    d->state.interruptionGeneration = interruptionGeneration + 1;
    d->homingAxes.clear();
    d->robots.clear();
    d->lastTime = d->clock();
    return true;
}

bool SimulationPlant::setConnected(const QString &id, bool connected, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->find(id, {}, error, false);
    if (!device)
        return false;
    if (device->connected && !connected)
        ++d->state.interruptionGeneration;
    device->connected = connected;
    if (!connected) {
        for (int i = 0; i < device->axes.size(); ++i)
            d->cancel(id, i, device->axes[i]);
        if (device->category == QLatin1String("robot"))
            d->cancelRobot(*device);
    }
    ++d->state.revision;
    return true;
}

bool SimulationPlant::enableAxis(const QString &id, int axis, bool enabled, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *value = d->axis(id, axis, error);
    if (!value)
        return false;
    value->enabled = enabled;
    if (!enabled)
        d->cancel(id, axis, *value);
    ++d->state.revision;
    return true;
}

bool SimulationPlant::moveAxis(const QString &id, int axis, double target, double speed, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    return d->startMove(id, axis, target, speed, false, error);
}

bool SimulationPlant::setAxisPosition(const QString &id, int axis, double position, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *value = d->axis(id, axis, error);
    if (!value)
        return false;
    if (d->state.emergencyStop)
        return fail(error, QStringLiteral("Simulation emergency stop is active"));
    if (!std::isfinite(position) || position < value->minimum || position > value->maximum)
        return fail(error, QStringLiteral("Axis position must be within soft limits %1..%2 mm")
                               .arg(value->minimum)
                               .arg(value->maximum));
    d->cancel(id, axis, *value);
    value->position = position;
    value->target = position;
    ++d->state.revision;
    return true;
}

bool SimulationPlant::homeAxis(const QString &id, int axis, double speed, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    return d->startMove(id, axis, 0, speed, true, error);
}

bool SimulationPlant::stopAxis(const QString &id, int axis, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *value = d->axis(id, axis, error);
    if (!value)
        return false;
    d->cancel(id, axis, *value);
    ++d->state.revision;
    return true;
}

void SimulationPlant::stopAllAxes() {
    QMutexLocker lock(&d->mutex);
    d->advance();
    for (auto &device : d->state.devices) {
        for (int i = 0; i < device.axes.size(); ++i)
            d->cancel(device.id, i, device.axes[i]);
        if (device.category == QLatin1String("robot"))
            d->cancelRobot(device);
    }
    ++d->state.revision;
}

bool SimulationPlant::enableRobot(const QString &id, bool enabled, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device)
        return false;
    if (!enabled)
        d->cancelRobot(*device);
    device->robot.enabled = enabled;
    ++d->state.revision;
    return true;
}

bool SimulationPlant::moveRobotJoints(const QString &id, const RobotJoints &target, double speedPercent,
                                      QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    return device && d->startRobotMove(*device, target, speedPercent, error);
}

bool SimulationPlant::moveRobotTcp(const QString &id, const RobotPose &target, double speedPercent,
                                   bool elbowPositive, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device)
        return false;
    RobotJoints joints;
    if (!robotInverse(target, elbowPositive, &joints, error))
        return false;
    return d->startRobotMove(*device, joints, speedPercent, error);
}

bool SimulationPlant::homeRobot(const QString &id, double speedPercent, QString *error) {
    return moveRobotJoints(id, RobotJoints{}, speedPercent, error);
}

bool SimulationPlant::stopRobot(const QString &id, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error, false);
    if (!device)
        return false;
    d->cancelRobot(*device);
    ++d->state.revision;
    return true;
}

bool SimulationPlant::setRobotGripper(const QString &id, bool closed, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device || !d->robotReady(*device, error))
        return false;
    auto &robot = device->robot;
    d->grip(robot, closed);
    robot.completedSequence = ++robot.commandSequence;
    ++d->state.revision;
    return true;
}

bool SimulationPlant::restockRobot(const QString &id, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device || !d->robotReady(*device, error))
        return false;
    if (device->robot.carryingPart)
        return fail(error, QStringLiteral("Release the held part before restocking"));
    if (device->robot.moving)
        return fail(error, QStringLiteral("Stop the robot before restocking"));
    device->robot.partLocation = QStringLiteral("feed");
    device->robot.partPose = {220, 40, 20, 0};
    ++d->state.revision;
    return true;
}

bool SimulationPlant::startRobotSort(const QString &id, const QString &outcome, double speedPercent,
                                     QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device || !d->robotReady(*device, error) || !d->validSpeed(speedPercent, error))
        return false;
    const QString bin = outcome.trimmed().toLower();
    if (bin != QLatin1String("ok") && bin != QLatin1String("ng"))
        return fail(error, QStringLiteral("Robot sort outcome must be OK or NG"));
    if (device->robot.moving || device->robot.carryingPart ||
        device->robot.partLocation != QLatin1String("feed"))
        return fail(error, QStringLiteral("Robot sorting requires a stopped robot and a part at feed"));
    QVector<Impl::RobotStep> steps;
    for (const RobotPose pose : {RobotPose{220, 40, 100, 0}, RobotPose{220, 40, 20, 0}}) {
        RobotJoints joints;
        if (!robotInverse(pose, true, &joints, error))
            return false;
        steps.append({joints, true, false, false});
    }
    steps.append({{}, false, true, true});
    steps.append({steps[0].target, true, false, false});
    const double y = bin == QLatin1String("ok") ? 200 : -200;
    for (const RobotPose pose : {RobotPose{80, y, 100, 0}, RobotPose{80, y, 20, 0}}) {
        RobotJoints joints;
        if (!robotInverse(pose, true, &joints, error))
            return false;
        steps.append({joints, true, false, false});
    }
    steps.append({{}, false, true, false});
    steps.append({steps[4].target, true, false, false});
    d->startRobotProgram(*device, QStringLiteral("sort_%1").arg(bin), std::move(steps), speedPercent);
    return true;
}

bool SimulationPlant::teachRobotPoint(const QString &id, const QString &name, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device || !d->teachingEditable(*device, error))
        return false;
    if (name.trimmed().isEmpty())
        return fail(error, QStringLiteral("Robot teaching point name must not be empty"));
    if (device->robot.taughtPoints.size() >= 100)
        return fail(error, QStringLiteral("Robot teaching is limited to 100 points"));
    device->robot.taughtPoints.append({name, device->robot.joints, device->robot.gripperClosed});
    ++d->state.revision;
    return true;
}

bool SimulationPlant::removeRobotPoint(const QString &id, int index, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device || !d->teachingEditable(*device, error))
        return false;
    if (index < 0 || index >= device->robot.taughtPoints.size())
        return fail(error, QStringLiteral("Robot teaching point index is out of range"));
    device->robot.taughtPoints.remove(index);
    ++d->state.revision;
    return true;
}

bool SimulationPlant::setRobotTeaching(const QString &id, const QVector<RobotWaypoint> &points,
                                       QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device || !d->teachingEditable(*device, error) || !d->validateTeaching(points, error))
        return false;
    device->robot.taughtPoints = points;
    ++d->state.revision;
    return true;
}

bool SimulationPlant::playRobotTeaching(const QString &id, double speedPercent, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    d->advance();
    auto *device = d->robot(id, error);
    if (!device || !d->robotReady(*device, error) || !d->validSpeed(speedPercent, error))
        return false;
    if (device->robot.moving)
        return fail(error, QStringLiteral("Stop the robot before teaching playback"));
    if (device->robot.taughtPoints.isEmpty())
        return fail(error, QStringLiteral("Robot teaching playback requires at least one point"));
    QVector<Impl::RobotStep> steps;
    for (const auto &point : device->robot.taughtPoints)
        steps.append({point.joints, true, true, point.gripperClosed});
    d->startRobotProgram(*device, QStringLiteral("teaching"), std::move(steps), speedPercent);
    return true;
}

bool SimulationPlant::setLight(const QString &id, int channel, bool enabled, int brightness, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    auto *device = d->find(id, {QStringLiteral("light")}, error);
    if (!device)
        return false;
    if (channel < 0 || channel >= device->lights.size())
        return fail(error, QStringLiteral("Light channel must be in 0..3"));
    if (brightness < 0 || brightness > 255)
        return fail(error, QStringLiteral("Brightness must be in 0..255"));
    device->lights[channel].enabled = enabled;
    device->lights[channel].brightness = brightness;
    ++d->state.revision;
    return true;
}

bool SimulationPlant::writeSignal(const QString &id, const QString &table, int address, quint16 value,
                                  QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    auto *device = d->find(id, {QStringLiteral("plc"), QStringLiteral("io")}, error);
    if (!device || !validateSignal(table, address, error))
        return false;
    if ((table == QLatin1String("coil") || table == QLatin1String("discrete")) && value > 1)
        return fail(error, QStringLiteral("Bit signals only accept 0 or 1"));
    if (table == QLatin1String("holding"))
        device->holdingRegisters[address] = value;
    else if (table == QLatin1String("input"))
        device->inputRegisters[address] = value;
    else if (table == QLatin1String("coil"))
        device->coils[address] = value != 0;
    else
        device->discreteInputs[address] = value != 0;
    ++d->state.revision;
    return true;
}

bool SimulationPlant::readSignal(const QString &id, const QString &table, int address, quint16 *value,
                                 QString *error) const {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    if (!value)
        return fail(error, QStringLiteral("Signal output value is required"));
    auto *device = d->find(id, {QStringLiteral("plc"), QStringLiteral("io")}, error);
    if (!device || !validateSignal(table, address, error))
        return false;
    if (table == QLatin1String("holding"))
        *value = device->holdingRegisters[address];
    else if (table == QLatin1String("input"))
        *value = device->inputRegisters[address];
    else if (table == QLatin1String("coil"))
        *value = device->coils[address] ? 1 : 0;
    else
        *value = device->discreteInputs[address] ? 1 : 0;
    return true;
}

bool SimulationPlant::queueTrigger(const QString &id, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    auto *device = d->find(id, {QStringLiteral("plc"), QStringLiteral("io")}, error);
    if (!device)
        return false;
    if (device->pendingTriggers >= 1024)
        return fail(error, QStringLiteral("Trigger queue is full (1024 pending)"));
    ++device->pendingTriggers;
    ++d->state.revision;
    return true;
}

bool SimulationPlant::takeTrigger(const QString &id, bool *ready, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    if (!ready)
        return fail(error, QStringLiteral("Trigger readiness output is required"));
    *ready = false;
    auto *device = d->find(id, {QStringLiteral("plc"), QStringLiteral("io")}, error);
    if (!device)
        return false;
    if (d->state.emergencyStop)
        return fail(error, QStringLiteral("Simulation emergency stop is active"));
    if (d->state.automaticTrigger)
        *ready = true;
    else if (device->pendingTriggers > 0) {
        --device->pendingTriggers;
        ++d->state.revision;
        *ready = true;
    } else
        *ready = device->discreteInputs[0];
    return true;
}

void SimulationPlant::setAutomaticTrigger(bool enabled) {
    QMutexLocker lock(&d->mutex);
    d->state.automaticTrigger = enabled;
    ++d->state.revision;
}
void SimulationPlant::setManualOverride(bool enabled) {
    QMutexLocker lock(&d->mutex);
    d->state.manualOverride = enabled;
    ++d->state.revision;
}
void SimulationPlant::setEmergencyStop(bool enabled) {
    QMutexLocker lock(&d->mutex);
    d->advance();
    if (enabled && !d->state.emergencyStop)
        ++d->state.interruptionGeneration;
    d->state.emergencyStop = enabled;
    for (auto &device : d->state.devices) {
        if (device.category == QLatin1String("robot")) {
            if (enabled) {
                d->cancelRobot(device);
                device.robot.fault = QStringLiteral("Emergency stop");
            } else if (device.robot.fault == QLatin1String("Emergency stop"))
                device.robot.fault.clear();
        }
        for (int i = 0; i < device.axes.size(); ++i) {
            auto &axis = device.axes[i];
            if (enabled) {
                d->cancel(device.id, i, axis);
                axis.fault = QStringLiteral("Emergency stop");
            } else if (axis.fault == QLatin1String("Emergency stop"))
                axis.fault.clear();
        }
    }
    ++d->state.revision;
}
bool SimulationPlant::setPartMode(const QString &mode, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    if (mode != QLatin1String("alternating") && mode != QLatin1String("good") &&
        mode != QLatin1String("ng") && mode != QLatin1String("missing"))
        return fail(error, QStringLiteral("Part mode must be alternating, good, ng or missing"));
    d->state.partMode = mode;
    ++d->state.revision;
    return true;
}
bool SimulationPlant::setPixelsPerMm(double value, QString *error) {
    QMutexLocker lock(&d->mutex);
    clearError(error);
    if (!std::isfinite(value) || value <= 0)
        return fail(error, QStringLiteral("Pixels per mm must be finite and greater than zero"));
    d->state.pixelsPerMm = value;
    ++d->state.revision;
    return true;
}

QImage SimulationPlant::renderFrame(const QString &cameraId, const QSize &size, quint64 sequence,
                                    QString *error) const {
    // Take one coherent scene, then release the mutex while allocating/rendering pixels.
    SimulationSnapshot scene;
    int channel = 0;
    double xPosition = 0, yPosition = 0;
    int lightLevel = 80;
    {
        QMutexLocker lock(&d->mutex);
        clearError(error);
        d->advance();
        auto *camera = d->find(cameraId, {QStringLiteral("camera")}, error);
        if (!camera)
            return {};
        if (d->state.emergencyStop) {
            fail(error, QStringLiteral("Simulation emergency stop is active"));
            return {};
        }
        if (size.width() < 16 || size.height() < 16 || size.width() > 8192 || size.height() > 8192 ||
            qint64(size.width()) * size.height() > 16 * 1024 * 1024) {
            fail(error,
                 QStringLiteral("Simulated frame must be 16..8192 pixels per axis, at most 16 megapixels"));
            return {};
        }
        QString motionId, lightId;
        const auto reference = [&](const QString &key, const QString &category, QString &id) {
            const auto field = camera->parameters.value(key);
            if (!field.isUndefined()) {
                if (!field.isString() || field.toString().isEmpty())
                    return fail(
                        error, QStringLiteral("Camera parameter '%1' must be a nonempty device ID").arg(key));
                id = field.toString();
                return d->find(id, {category}, error) != nullptr;
            }
            for (const auto &device : d->state.devices)
                if (device.category == category) {
                    id = device.id;
                    return d->find(id, {category}, error) != nullptr;
                }
            return true;
        };
        if (!reference(QStringLiteral("motionDeviceId"), QStringLiteral("motion"), motionId) ||
            !reference(QStringLiteral("lightDeviceId"), QStringLiteral("light"), lightId))
            return {};
        const auto channelField = camera->parameters.value(QStringLiteral("lightChannel"));
        if (!channelField.isUndefined()) {
            const double value = channelField.toDouble(-1);
            if (!channelField.isDouble() || !std::isfinite(value) || std::floor(value) != value ||
                value < 0 || value > 3) {
                fail(error, QStringLiteral("Camera lightChannel must be an integer in 0..3"));
                return {};
            }
            channel = int(value);
        }
        if (!motionId.isEmpty()) {
            const auto *motion = d->find(motionId, {QStringLiteral("motion")}, error);
            xPosition = motion->axes[0].position;
            yPosition = motion->axes[1].position;
        }
        if (!lightId.isEmpty()) {
            const auto *light = d->find(lightId, {QStringLiteral("light")}, error);
            const auto &value = light->lights[channel];
            lightLevel = value.enabled ? value.brightness : 0;
        }
        scene = d->state;
    }
    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) {
        fail(error, QStringLiteral("Could not allocate simulated camera frame"));
        return {};
    }
    image.fill(qRgb(24, 28, 36));
    if (scene.partMode == QLatin1String("missing"))
        return image;
    const bool ng = scene.partMode == QLatin1String("ng") ||
                    (scene.partMode == QLatin1String("alternating") && sequence > 0 && sequence % 5 == 0);
    const int diameter = std::min(ng ? 460 : 400, std::min(size.width(), size.height()) - 8);
    const int offsetX = (int(sequence % 7) - 3) * 2;
    const int offsetY = (int(sequence % 5) - 2) * 2;
    const double left = std::clamp((size.width() - diameter) / 2 + offsetX, 0, size.width() - diameter) +
                        xPosition * scene.pixelsPerMm;
    const double top = std::clamp((size.height() - diameter) / 2 + offsetY, 0, size.height() - diameter) +
                       yPosition * scene.pixelsPerMm;
    const double radius = diameter / 2.0;
    // Calibration may place the whole part outside the sensor, without clamping its location.
    if (!std::isfinite(left) || !std::isfinite(top) || left >= size.width() || top >= size.height() ||
        left + diameter <= 0 || top + diameter <= 0)
        return image;
    const int firstX = int(std::max(0.0, std::floor(left)));
    const int lastX = int(std::min(double(size.width()), std::ceil(left + diameter)));
    const int firstY = int(std::max(0.0, std::floor(top)));
    const int lastY = int(std::min(double(size.height()), std::ceil(top + diameter)));
    const int brightness = std::clamp(int(std::lround(236.0 * lightLevel / 80.0)), 0, 255);
    const QRgb color = qRgb(brightness, brightness, brightness);
    for (int y = firstY; y < lastY; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        const double dy = y + 0.5 - top - radius;
        for (int x = firstX; x < lastX; ++x) {
            const double dx = x + 0.5 - left - radius;
            if (dx * dx + dy * dy <= radius * radius)
                line[x] = color;
        }
    }
    return image;
}
} // namespace ivs
