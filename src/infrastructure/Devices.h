#pragma once

#include "domain/Types.h"

#include <memory>

namespace ivs {

class SimulationPlant;

struct DeviceDescriptor {
    QString category;
    QString model;
    QString backend;
    QString status;
    QString interfaceName;
};

QList<DeviceDescriptor> hardwareCatalog();

class SimulatedCamera final {
  public:
    QImage capture(const QSize &size, quint64 sequence, QString *error = nullptr);
};

enum class TriggerReadiness { Ready, Waiting, Failed };

// Owned by the inspection worker; sockets and adapters stay on that thread.
class DeviceGateway final {
  public:
    explicit DeviceGateway(std::shared_ptr<SimulationPlant> plant = {});
    ~DeviceGateway();
    DeviceGateway(const DeviceGateway &) = delete;
    DeviceGateway &operator=(const DeviceGateway &) = delete;

    bool configure(const QList<DeviceConfig> &devices, QString *error = nullptr);
    bool connectAll(QString *error = nullptr);
    void disconnectAll();
    bool execute(const FlowNode &node, QString *error = nullptr);
    // Sorting receives the committed inspection outcome from the worker, never node parameters.
    bool executeRobotSort(const FlowNode &node, const QString &committedOutcome, QString *error = nullptr);
    TriggerReadiness robotReady(const FlowNode &node, QString *error = nullptr);
    std::shared_ptr<SimulationPlant> simulationPlant() const;
    bool simulationReady(QString *error = nullptr) const;
    TriggerReadiness motionReady(const FlowNode &node, QString *error = nullptr);
    TriggerReadiness triggerReady(const FlowNode &node, QString *error = nullptr);
    QImage capture(const QSize &size, quint64 sequence, QString *error = nullptr);
    QImage capture(const QSize &size, quint64 sequence, const QString &deviceId, QString *error = nullptr);

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace ivs
