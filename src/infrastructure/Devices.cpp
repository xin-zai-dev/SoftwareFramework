#include "Devices.h"

#include "HikMvsCamera.h"
#include "ModbusTcpClient.h"
#include "SimulationPlant.h"

#include <QHash>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace ivs {

namespace {
void clearError(QString *error) {
    if (error)
        error->clear();
}
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}

QString normalizedBackend(QString backend) {
    backend = backend.trimmed().toLower();
    if (backend == QLatin1String("modbus") || backend == QLatin1String("modbus_tcp"))
        return QStringLiteral("modbus-tcp");
    return backend;
}

bool jsonInteger(const QJsonObject &object, const QString &key, int fallback, int minimum, int maximum,
                 int &value, QString *error) {
    const auto field = object.value(key);
    if (field.isUndefined()) {
        value = fallback;
        return true;
    }
    if (!field.isDouble() || !std::isfinite(field.toDouble()) ||
        field.toDouble() != std::floor(field.toDouble()) || field.toDouble() < minimum ||
        field.toDouble() > maximum)
        return fail(
            error,
            QStringLiteral("Parameter '%1' must be an integer in %2..%3").arg(key).arg(minimum).arg(maximum));
    value = field.toInt();
    return true;
}

bool jsonNumber(const QJsonObject &object, const QString &key, double fallback, double minimum,
                double maximum, double &value, QString *error) {
    const auto field = object.value(key);
    if (field.isUndefined()) {
        value = fallback;
        return true;
    }
    if (!field.isDouble() || !std::isfinite(field.toDouble()) || field.toDouble() < minimum ||
        field.toDouble() > maximum)
        return fail(error, QStringLiteral("Parameter '%1' must be a finite number in %2..%3")
                               .arg(key)
                               .arg(minimum)
                               .arg(maximum));
    value = field.toDouble();
    return true;
}

bool signalFunction(const FlowNode &node, const QString &fallback, QString *function, QString *error) {
    const auto field = node.parameters.value(QStringLiteral("function"));
    if (!field.isUndefined() && !field.isString())
        return fail(error, QStringLiteral("Parameter 'function' must be a string"));
    *function = field.toString(fallback).trimmed().toLower();
    return true;
}

bool jsonBoolean(const QJsonObject &object, const QString &key, bool fallback, bool *value, QString *error) {
    const auto field = object.value(key);
    if (!field.isUndefined() && !field.isBool())
        return fail(error, QStringLiteral("Parameter '%1' must be a boolean").arg(key));
    *value = field.toBool(fallback);
    return true;
}

QString robotCommandKey(const FlowNode &node) {
    return node.id + QChar(0) + node.type;
}

const SimDeviceState *robotState(const SimulationSnapshot &snapshot, const QString &id) {
    for (const auto &device : snapshot.devices)
        if (device.id == id && device.category == QLatin1String("robot"))
            return &device;
    return nullptr;
}

bool robotParameters(const FlowNode &node, QString *error) {
    QSet<QString> allowed{QStringLiteral("deviceId"), QStringLiteral("timeoutMs")};
    if (node.type == QLatin1String("robot_move"))
        allowed.unite({QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z"), QStringLiteral("r"),
                       QStringLiteral("elbowPositive"), QStringLiteral("speedPercent")});
    else if (node.type == QLatin1String("robot_grip"))
        allowed.insert(QStringLiteral("closed"));
    else if (node.type == QLatin1String("robot_sort"))
        allowed.unite({QStringLiteral("speedPercent"), QStringLiteral("restock")});
    else
        return fail(error, QStringLiteral("Unsupported robot node type '%1'").arg(node.type));
    for (auto field = node.parameters.begin(); field != node.parameters.end(); ++field)
        if (!allowed.contains(field.key()))
            return fail(error, QStringLiteral("Unsupported robot parameter '%1'").arg(field.key()));
    return true;
}

bool nodeDeviceId(const FlowNode &node, QString *deviceId, QString *error) {
    const auto field = node.parameters.value(QStringLiteral("deviceId"));
    if (!field.isUndefined() && !field.isString())
        return fail(error, QStringLiteral("Parameter 'deviceId' must be a string when present"));
    *deviceId = field.toString();
    return true;
}
} // namespace

QList<DeviceDescriptor> hardwareCatalog() {
    return {{"camera", "Simulated camera", "simulated", "available / simulation", "Synthetic RGB image"},
            {"motion", "Simulated motion", "simulated", "available / simulation", "Software"},
            {"robot", "SCARA 四轴仿真机械臂", "simulated", "available / simulation", "Joint-space SCARA"},
            {"light", "Simulated lighting", "simulated", "available / simulation", "Software"},
            {"plc", "Generic Modbus TCP PLC", "modbus-tcp", "implemented / hardware validation required",
             "Ethernet / Modbus TCP"},
            {"io", "Generic Modbus TCP I/O", "modbus-tcp", "implemented / hardware validation required",
             "Ethernet / Modbus TCP"},
            {"camera", "Baumer VCXG-51M", "baumer", "unavailable / vendor SDK adapter planned",
             "GigE Vision / vendor SDK"},
            {"camera", "Hikrobot MV-CS050-10GM", "mvs", "unavailable / MVS adapter hook only",
             "GigE Vision / MVS SDK"},
            {"motion", "GTS-400-PV-PCIe-00", "googol", "unavailable / vendor SDK adapter planned",
             "PCIe / vendor SDK"},
            {"motion", "DMC3400A-PCIe", "leisai", "unavailable / vendor SDK adapter planned",
             "PCIe / vendor SDK"},
            {"light", "OPT-DPA2024EC-4", "opt", "unavailable / protocol adapter planned",
             "Ethernet / vendor protocol"},
            {"light", "CCS PD3-3024-3-EI(A)", "ccs", "unavailable / protocol adapter planned",
             "Ethernet / vendor protocol"},
            {"plc", "Schneider TM221CE16T", "modbus-tcp",
             "generic protocol implemented / model mapping unverified", "Ethernet / Modbus TCP"},
            {"io", "ADAM-6050-D1", "modbus-tcp", "generic protocol implemented / model mapping unverified",
             "Ethernet / Modbus TCP"}};
}

QImage SimulatedCamera::capture(const QSize &size, quint64 sequence, QString *error) {
    clearError(error);
    if (size.width() < 16 || size.height() < 16 || size.width() > 8192 || size.height() > 8192 ||
        qint64(size.width()) * size.height() > 64 * 1024 * 1024) {
        fail(error,
             QStringLiteral(
                 "Simulated camera resolution must be 16..8192 pixels per axis, at most 64 megapixels"));
        return {};
    }
    QImage image(size, QImage::Format_RGB32);
    if (image.isNull()) {
        fail(error, QStringLiteral("Could not allocate simulated camera frame"));
        return {};
    }
    image.fill(qRgb(24, 28, 36));
    const int nominalDiameter = sequence > 0 && sequence % 5 == 0 ? 460 : 400;
    const int diameter = std::min(nominalDiameter, std::min(size.width(), size.height()) - 8);
    const int offsetX = (int(sequence % 7) - 3) * 2;
    const int offsetY = (int(sequence % 5) - 2) * 2;
    const int left = std::clamp((size.width() - diameter) / 2 + offsetX, 0, size.width() - diameter);
    const int top = std::clamp((size.height() - diameter) / 2 + offsetY, 0, size.height() - diameter);
    const double radius = diameter / 2.0;
    for (int y = 0; y < diameter; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(top + y));
        const double dy = y + 0.5 - radius;
        for (int x = 0; x < diameter; ++x) {
            const double dx = x + 0.5 - radius;
            if (dx * dx + dy * dy <= radius * radius)
                line[left + x] = qRgb(236, 236, 236);
        }
    }
    return image;
}

struct DeviceGateway::Impl {
    struct Entry {
        DeviceConfig config;
        std::unique_ptr<ModbusTcpClient> modbus;
    };
    std::vector<Entry> entries;
    bool connected = false;
    std::shared_ptr<SimulationPlant> plant;
    struct RobotCommand {
        QString deviceId;
        quint64 sequence = 0;
    };
    QHash<QString, RobotCommand> robotCommands;

    bool available(const Entry &entry, QString *error) const {
        const auto state = plant->snapshot();
        if (state.emergencyStop)
            return fail(error, QStringLiteral("Simulation emergency stop is active"));
        for (const auto &device : state.devices)
            if (device.id == entry.config.id)
                return device.connected ||
                       fail(error, QStringLiteral("Simulated device '%1' is disconnected").arg(device.id));
        return fail(error, QStringLiteral("Simulated device '%1' is not configured").arg(entry.config.id));
    }

    bool issueRobot(const FlowNode &node, const QString &outcome, QString *error) {
        robotCommands.remove(robotCommandKey(node));
        QString deviceId;
        if (!nodeDeviceId(node, &deviceId, error) || !robotParameters(node, error))
            return false;
        auto *entry = find(deviceId, {QStringLiteral("robot")}, error);
        if (!entry)
            return false;
        if (entry->config.backend != QLatin1String("simulated"))
            return fail(error, QStringLiteral("Robots support the simulated backend only"));
        if (!available(*entry, error))
            return false;
        int timeout = 0;
        if (!jsonInteger(node.parameters, QStringLiteral("timeoutMs"),
                         node.type == QLatin1String("robot_sort") ? 60000 : 30000, 1, 60000, timeout, error))
            return false;
        double speed = 60;
        if (node.type != QLatin1String("robot_grip") &&
            !jsonNumber(node.parameters, QStringLiteral("speedPercent"), 60, 1, 100, speed, error))
            return false;
        const auto before = plant->snapshot();
        const auto *prior = robotState(before, entry->config.id);
        if (!prior)
            return fail(error, QStringLiteral("Robot state is not configured"));
        const quint64 expected = prior->robot.commandSequence + 1;
        if (node.type == QLatin1String("robot_move")) {
            RobotPose target{220, 40, 100, 0};
            bool elbowPositive = true;
            const double bound = std::numeric_limits<double>::max();
            if (!jsonNumber(node.parameters, QStringLiteral("x"), 220, -bound, bound, target.x, error) ||
                !jsonNumber(node.parameters, QStringLiteral("y"), 40, -bound, bound, target.y, error) ||
                !jsonNumber(node.parameters, QStringLiteral("z"), 100, 0, 150, target.z, error) ||
                !jsonNumber(node.parameters, QStringLiteral("r"), 0, -bound, bound, target.r, error) ||
                !jsonBoolean(node.parameters, QStringLiteral("elbowPositive"), true, &elbowPositive, error))
                return false;
            if (!plant->moveRobotTcp(entry->config.id, target, speed, elbowPositive, error))
                return false;
        } else if (node.type == QLatin1String("robot_grip")) {
            bool closed = true;
            if (!jsonBoolean(node.parameters, QStringLiteral("closed"), true, &closed, error) ||
                !plant->setRobotGripper(entry->config.id, closed, error))
                return false;
        } else {
            bool restock = false;
            if (outcome != QLatin1String("OK") && outcome != QLatin1String("NG"))
                return fail(error, QStringLiteral("Robot sort requires a committed OK or NG outcome"));
            if (!jsonBoolean(node.parameters, QStringLiteral("restock"), false, &restock, error))
                return false;
            if (restock && !plant->restockRobot(entry->config.id, error))
                return false;
            if (!plant->startRobotSort(entry->config.id, outcome, speed, error))
                return false;
        }
        const auto after = plant->snapshot();
        const auto *issued = robotState(after, entry->config.id);
        if (!issued || issued->robot.commandSequence != expected)
            return fail(error, QStringLiteral("Robot command was concurrently replaced"));
        robotCommands.insert(robotCommandKey(node), {entry->config.id, expected});
        return true;
    }

    Entry *find(const QString &id, const QStringList &categories, QString *error) {
        for (auto &entry : entries) {
            if ((!id.isEmpty() && entry.config.id == id) ||
                (id.isEmpty() && categories.contains(entry.config.category))) {
                if (!categories.contains(entry.config.category)) {
                    fail(error,
                         QStringLiteral("Device '%1' has category '%2'; node requires %3")
                             .arg(entry.config.id, entry.config.category, categories.join(QLatin1Char('/'))));
                    return nullptr;
                }
                return &entry;
            }
        }
        fail(error, id.isEmpty() ? QStringLiteral("No configured device for category %1")
                                       .arg(categories.join(QLatin1Char('/')))
                                 : QStringLiteral("Device '%1' does not exist").arg(id));
        return nullptr;
    }
};

DeviceGateway::DeviceGateway(std::shared_ptr<SimulationPlant> plant) : d(std::make_unique<Impl>()) {
    d->plant = plant ? std::move(plant) : std::make_shared<SimulationPlant>();
}
DeviceGateway::~DeviceGateway() {
    disconnectAll();
}

bool DeviceGateway::configure(const QList<DeviceConfig> &devices, QString *error) {
    clearError(error);
    QSet<QString> ids;
    std::vector<Impl::Entry> candidate;
    const QSet<QString> categories{"camera", "motion", "light", "plc", "io", "robot"};
    for (auto config : devices) {
        config.id = config.id.trimmed();
        config.category = config.category.trimmed().toLower();
        config.backend = normalizedBackend(config.backend);
        if (config.id.isEmpty() || ids.contains(config.id))
            return fail(error, QStringLiteral("Device IDs must be nonempty and unique: '%1'").arg(config.id));
        if (!categories.contains(config.category))
            return fail(error, QStringLiteral("Unsupported device category '%1'").arg(config.category));
        ids.insert(config.id);
        if (config.category == QLatin1String("robot") && config.backend != QLatin1String("simulated"))
            return fail(error, QStringLiteral("Robots support the simulated backend only"));
        if (config.backend == QLatin1String("modbus-tcp")) {
            if (config.category != QLatin1String("plc") && config.category != QLatin1String("io"))
                return fail(error, QStringLiteral("Modbus TCP supports PLC and I/O categories only"));
            int timeout = 0;
            if (!jsonInteger(config.parameters, QStringLiteral("timeoutMs"), 1000, 1, 60000, timeout, error))
                return false;
            if (config.host.trimmed().isEmpty() || config.port < 1 || config.port > 65535 ||
                config.unitId < 0 || config.unitId > 255)
                return fail(error,
                            QStringLiteral("Invalid Modbus configuration for '%1': host, port or unit ID")
                                .arg(config.id));
        } else if (config.backend == QLatin1String("mvs")) {
            return fail(error, HikMvsCamera::unavailableReason());
        } else if (config.backend != QLatin1String("simulated")) {
            return fail(error, QStringLiteral("Unsupported backend '%1' for '%2': vendor SDK/protocol "
                                              "adapter is unavailable; choose simulated for demonstration")
                                   .arg(config.backend, config.id));
        }
        candidate.push_back({config, nullptr});
    }
    QList<DeviceConfig> normalized;
    for (const auto &entry : candidate)
        normalized.append(entry.config);
    if (!d->plant->configure(normalized, error))
        return false;
    disconnectAll();
    d->entries = std::move(candidate);
    return true;
}

bool DeviceGateway::connectAll(QString *error) {
    clearError(error);
    disconnectAll();
    for (auto &entry : d->entries) {
        if (entry.config.backend != QLatin1String("modbus-tcp"))
            continue;
        entry.modbus = std::make_unique<ModbusTcpClient>();
        const int timeout = entry.config.parameters.value(QStringLiteral("timeoutMs")).toInt(1000);
        if (!entry.modbus->connectToHost(entry.config.host, entry.config.port, entry.config.unitId, timeout,
                                         error)) {
            if (error)
                *error = QStringLiteral("Device '%1': %2").arg(entry.config.id, *error);
            disconnectAll();
            return false;
        }
    }
    d->connected = true;
    return true;
}

void DeviceGateway::disconnectAll() {
    d->connected = false;
    d->robotCommands.clear();
    for (auto &entry : d->entries)
        entry.modbus.reset();
}

bool DeviceGateway::execute(const FlowNode &node, QString *error) {
    clearError(error);
    if (!node.enabled)
        return true;
    if (!d->connected)
        return fail(error, QStringLiteral("Devices are not connected"));
    QString deviceId;
    if (!nodeDeviceId(node, &deviceId, error))
        return false;
    const QString type = node.type.trimmed().toLower();
    if (type == QLatin1String("robot_move") || type == QLatin1String("robot_grip") ||
        type == QLatin1String("robot_sort"))
        return d->issueRobot(node, QString(), error);
    if (type == QLatin1String("trigger")) {
        const auto readiness = triggerReady(node, error);
        if (readiness == TriggerReadiness::Waiting)
            return fail(error, QStringLiteral("Trigger condition not met"));
        return readiness == TriggerReadiness::Ready;
    }
    QStringList categories;
    if (type == QLatin1String("motion") || type == QLatin1String("move"))
        categories = {QStringLiteral("motion")};
    else if (type == QLatin1String("light") || type == QLatin1String("lighting"))
        categories = {QStringLiteral("light")};
    else if (type == QLatin1String("output"))
        categories = {QStringLiteral("plc"), QStringLiteral("io")};
    else
        return fail(error, QStringLiteral("Unsupported device node type '%1'").arg(node.type));

    auto *entry = d->find(deviceId, categories, error);
    if (!entry)
        return false;
    if (entry->config.backend == QLatin1String("simulated")) {
        if (!d->available(*entry, error))
            return false;
        if (categories.contains(QStringLiteral("motion"))) {
            int axis = 0, timeout = 0;
            double position = 0, speed = 25;
            if (!jsonInteger(node.parameters, QStringLiteral("axis"), 0, 0, 3, axis, error) ||
                !jsonInteger(node.parameters, QStringLiteral("timeoutMs"), 10000, 1, 60000, timeout, error) ||
                !jsonNumber(node.parameters, QStringLiteral("position"), 0,
                            -std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), position,
                            error) ||
                !jsonNumber(node.parameters, QStringLiteral("speed"), 25, 0, 1000, speed, error))
                return false;
            if (speed <= 0)
                return fail(error, QStringLiteral("Parameter 'speed' must be greater than zero"));
            const auto snapshot = d->plant->snapshot();
            if (snapshot.manualOverride)
                return motionReady(node, error) != TriggerReadiness::Failed;
            return d->plant->moveAxis(entry->config.id, axis, position, speed, error);
        }
        if (categories.contains(QStringLiteral("light"))) {
            int channel = 0, brightness = 0;
            if (!jsonInteger(node.parameters, QStringLiteral("channel"), 0, 0, 3, channel, error) ||
                !jsonInteger(node.parameters, QStringLiteral("brightness"), 80, 0, 255, brightness, error))
                return false;
            const auto enabled = node.parameters.value(QStringLiteral("enabled"));
            if (!enabled.isUndefined() && !enabled.isBool())
                return fail(error, QStringLiteral("Parameter 'enabled' must be a boolean"));
            if (d->plant->snapshot().manualOverride)
                return true;
            return d->plant->setLight(entry->config.id, channel, enabled.toBool(true), brightness, error);
        }
        int address = 0, value = 0;
        QString function;
        if (!signalFunction(node, QStringLiteral("register"), &function, error))
            return false;
        if (function != QLatin1String("coil") && function != QLatin1String("register") &&
            function != QLatin1String("holding"))
            return fail(error, QStringLiteral("Output function must be 'register', 'holding' or 'coil'"));
        if (!jsonInteger(node.parameters, QStringLiteral("address"), 0, 0,
                         function == QLatin1String("coil") ? 15 : 63, address, error) ||
            !jsonInteger(node.parameters, QStringLiteral("value"), 0, 0,
                         function == QLatin1String("coil") ? 1 : 65535, value, error))
            return false;
        return d->plant->writeSignal(
            entry->config.id, function == QLatin1String("register") ? QStringLiteral("holding") : function,
            address, quint16(value), error);
    }
    if (!entry->modbus)
        return fail(error, QStringLiteral("No connected Modbus adapter for '%1'").arg(entry->config.id));
    int address = 0;
    if (!jsonInteger(node.parameters, QStringLiteral("address"), 0, 0, 65535, address, error))
        return false;
    const QString function = node.parameters.value(QStringLiteral("function"))
                                 .toString(type == QLatin1String("output") ? QStringLiteral("register")
                                                                           : QStringLiteral("holding"))
                                 .toLower();
    if (type == QLatin1String("output")) {
        int value = 0;
        if (!jsonInteger(node.parameters, QStringLiteral("value"), 0, 0,
                         function == QLatin1String("coil") ? 1 : 65535, value, error))
            return false;
        if (function == QLatin1String("coil"))
            return entry->modbus->writeSingleCoil(quint16(address), value != 0, error);
        if (function == QLatin1String("register") || function == QLatin1String("holding"))
            return entry->modbus->writeSingleRegister(quint16(address), quint16(value), error);
        return fail(error, QStringLiteral("Output function must be 'register' or 'coil'"));
    }
    return fail(error, QStringLiteral("Unsupported device operation"));
}

bool DeviceGateway::executeRobotSort(const FlowNode &node, const QString &committedOutcome, QString *error) {
    clearError(error);
    if (!node.enabled)
        return true;
    if (!d->connected)
        return fail(error, QStringLiteral("Devices are not connected"));
    if (node.type != QLatin1String("robot_sort"))
        return fail(error, QStringLiteral("Committed sorting requires a robot_sort node"));
    return d->issueRobot(node, committedOutcome, error);
}

TriggerReadiness DeviceGateway::robotReady(const FlowNode &node, QString *error) {
    clearError(error);
    if (!node.enabled)
        return TriggerReadiness::Ready;
    if (!d->connected) {
        fail(error, QStringLiteral("Devices are not connected"));
        return TriggerReadiness::Failed;
    }
    QString deviceId;
    if (!nodeDeviceId(node, &deviceId, error) || !robotParameters(node, error))
        return TriggerReadiness::Failed;
    auto *entry = d->find(deviceId, {QStringLiteral("robot")}, error);
    if (!entry || !d->available(*entry, error))
        return TriggerReadiness::Failed;
    const auto command = d->robotCommands.constFind(robotCommandKey(node));
    if (command == d->robotCommands.cend() || command->deviceId != entry->config.id) {
        fail(error, QStringLiteral("Node has no issued command for this robot"));
        return TriggerReadiness::Failed;
    }
    const auto snapshot = d->plant->snapshot();
    const auto *device = robotState(snapshot, entry->config.id);
    if (!device) {
        fail(error, QStringLiteral("Robot state is not configured"));
        return TriggerReadiness::Failed;
    }
    const auto &robot = device->robot;
    if (!robot.enabled || !robot.fault.isEmpty()) {
        fail(error, robot.fault.isEmpty() ? QStringLiteral("Robot is disabled") : robot.fault);
        return TriggerReadiness::Failed;
    }
    if (robot.commandSequence != command->sequence || robot.interruptedSequence >= command->sequence) {
        fail(error, QStringLiteral("Robot command was stopped or replaced"));
        return TriggerReadiness::Failed;
    }
    return robot.completedSequence == command->sequence && !robot.moving && !robot.programRunning
               ? TriggerReadiness::Ready
               : TriggerReadiness::Waiting;
}

std::shared_ptr<SimulationPlant> DeviceGateway::simulationPlant() const {
    return d->plant;
}

bool DeviceGateway::simulationReady(QString *error) const {
    clearError(error);
    for (const auto &entry : d->entries)
        if (entry.config.backend == QLatin1String("simulated") && !d->available(entry, error))
            return false;
    return true;
}

TriggerReadiness DeviceGateway::motionReady(const FlowNode &node, QString *error) {
    clearError(error);
    if (!node.enabled)
        return TriggerReadiness::Ready;
    if (!d->connected) {
        fail(error, QStringLiteral("Devices are not connected"));
        return TriggerReadiness::Failed;
    }
    QString deviceId;
    int axis = 0;
    if (!nodeDeviceId(node, &deviceId, error) ||
        !jsonInteger(node.parameters, QStringLiteral("axis"), 0, 0, 3, axis, error))
        return TriggerReadiness::Failed;
    auto *entry = d->find(deviceId, {QStringLiteral("motion")}, error);
    if (!entry)
        return TriggerReadiness::Failed;
    if (entry->config.backend != QLatin1String("simulated")) {
        fail(error, QStringLiteral("No motion readiness adapter for '%1'").arg(entry->config.id));
        return TriggerReadiness::Failed;
    }
    if (!d->available(*entry, error))
        return TriggerReadiness::Failed;
    const auto snapshot = d->plant->snapshot();
    for (const auto &device : snapshot.devices)
        if (device.id == entry->config.id) {
            if (axis >= device.axes.size()) {
                fail(error, QStringLiteral("Motion axis is not configured"));
                return TriggerReadiness::Failed;
            }
            const auto &state = device.axes.at(axis);
            if (!state.enabled || !state.fault.isEmpty()) {
                fail(error, state.fault.isEmpty() ? QStringLiteral("Motion axis is disabled") : state.fault);
                return TriggerReadiness::Failed;
            }
            if (!snapshot.manualOverride) {
                double requested = 0;
                if (!jsonNumber(node.parameters, QStringLiteral("position"), 0,
                                -std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                requested, error))
                    return TriggerReadiness::Failed;
                constexpr double tolerance = 1e-6;
                if (std::abs(state.target - requested) > tolerance ||
                    (!state.moving && std::abs(state.position - requested) > tolerance)) {
                    fail(error, QStringLiteral("Motion was stopped or retargeted before reaching %1 mm")
                                    .arg(requested));
                    return TriggerReadiness::Failed;
                }
            }
            return state.moving ? TriggerReadiness::Waiting : TriggerReadiness::Ready;
        }
    fail(error, QStringLiteral("Motion device is not configured"));
    return TriggerReadiness::Failed;
}

TriggerReadiness DeviceGateway::triggerReady(const FlowNode &node, QString *error) {
    clearError(error);
    if (!node.enabled)
        return TriggerReadiness::Ready;
    if (!d->connected) {
        fail(error, QStringLiteral("Devices are not connected"));
        return TriggerReadiness::Failed;
    }
    QString deviceId;
    if (!nodeDeviceId(node, &deviceId, error))
        return TriggerReadiness::Failed;
    auto *entry = d->find(deviceId, {QStringLiteral("plc"), QStringLiteral("io")}, error);
    if (!entry)
        return TriggerReadiness::Failed;
    if (entry->config.backend == QLatin1String("simulated")) {
        if (!d->available(*entry, error))
            return TriggerReadiness::Failed;
        if (!node.parameters.contains(QStringLiteral("requireValue"))) {
            bool ready = false;
            if (!d->plant->takeTrigger(entry->config.id, &ready, error))
                return TriggerReadiness::Failed;
            return ready ? TriggerReadiness::Ready : TriggerReadiness::Waiting;
        }
        QString function;
        if (!signalFunction(node, QStringLiteral("holding"), &function, error))
            return TriggerReadiness::Failed;
        if (!QStringList{QStringLiteral("holding"), QStringLiteral("input"), QStringLiteral("coil"),
                         QStringLiteral("discrete")}
                 .contains(function)) {
            fail(error, QStringLiteral("Trigger function must be 'holding', 'input', 'coil' or 'discrete'"));
            return TriggerReadiness::Failed;
        }
        const bool bit = function == QLatin1String("coil") || function == QLatin1String("discrete");
        int address = 0, required = 0;
        if (!jsonInteger(node.parameters, QStringLiteral("address"), 0, 0, bit ? 15 : 63, address, error) ||
            !jsonInteger(node.parameters, QStringLiteral("requireValue"), 0, 0, bit ? 1 : 65535, required,
                         error))
            return TriggerReadiness::Failed;
        quint16 actual = 0;
        if (!d->plant->readSignal(entry->config.id, function, address, &actual, error))
            return TriggerReadiness::Failed;
        return actual == required ? TriggerReadiness::Ready : TriggerReadiness::Waiting;
    }
    if (!entry->modbus) {
        fail(error, QStringLiteral("Modbus adapter is not connected"));
        return TriggerReadiness::Failed;
    }
    int address = 0, required = 0;
    if (!jsonInteger(node.parameters, QStringLiteral("address"), 0, 0, 65535, address, error))
        return TriggerReadiness::Failed;
    const QString function =
        node.parameters.value(QStringLiteral("function")).toString(QStringLiteral("holding")).toLower();
    if (node.parameters.contains(QStringLiteral("requireValue")) &&
        !jsonInteger(node.parameters, QStringLiteral("requireValue"), 0, 0,
                     function == QLatin1String("coil") || function == QLatin1String("discrete") ? 1 : 65535,
                     required, error))
        return TriggerReadiness::Failed;
    int actual = 0;
    if (function == QLatin1String("coil") || function == QLatin1String("discrete")) {
        const auto values = function == QLatin1String("coil")
                                ? entry->modbus->readCoils(quint16(address), 1, error)
                                : entry->modbus->readDiscreteInputs(quint16(address), 1, error);
        if (values.isEmpty())
            return TriggerReadiness::Failed;
        actual = values.first() ? 1 : 0;
    } else if (function == QLatin1String("holding") || function == QLatin1String("input")) {
        const auto values = function == QLatin1String("holding")
                                ? entry->modbus->readHoldingRegisters(quint16(address), 1, error)
                                : entry->modbus->readInputRegisters(quint16(address), 1, error);
        if (values.isEmpty())
            return TriggerReadiness::Failed;
        actual = values.first();
    } else {
        fail(error, QStringLiteral("Trigger function must be 'holding', 'input', 'coil' or 'discrete'"));
        return TriggerReadiness::Failed;
    }
    if (node.parameters.contains(QStringLiteral("requireValue"))) {
        if (actual != required)
            return TriggerReadiness::Waiting;
    }
    return TriggerReadiness::Ready;
}

QImage DeviceGateway::capture(const QSize &size, quint64 sequence, QString *error) {
    return capture(size, sequence, QString(), error);
}

QImage DeviceGateway::capture(const QSize &size, quint64 sequence, const QString &deviceId, QString *error) {
    clearError(error);
    if (!d->connected) {
        fail(error, QStringLiteral("Devices are not connected"));
        return {};
    }
    auto *entry = d->find(deviceId, {QStringLiteral("camera")}, error);
    if (!entry)
        return {};
    if (entry->config.backend == QLatin1String("simulated")) {
        if (!d->available(*entry, error))
            return {};
        return d->plant->renderFrame(entry->config.id, size, sequence, error);
    }
    fail(error, QStringLiteral("No available camera adapter for '%1'").arg(entry->config.id));
    return {};
}

} // namespace ivs
