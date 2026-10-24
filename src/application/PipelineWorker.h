#pragma once

#include "domain/Types.h"

#include <QObject>
#include <memory>

namespace ivs {

class SimulationPlant;

// Move to the inspection thread before calling start(); all owned I/O stays there.
class PipelineWorker final : public QObject {
    Q_OBJECT
  public:
    explicit PipelineWorker(QObject *parent = nullptr);
    ~PipelineWorker() override;
    // Call on the owning thread while stopped, before starting the worker.
    bool setSimulationPlant(std::shared_ptr<SimulationPlant> plant);

  public slots:
    void start(const ivs::ProjectConfig &project, const QString &workspace, bool singleShot = false);
    void stop();
    void shutdown();

  signals:
    void stateChanged(const QString &state);
    void resultReady(const ivs::InspectionResult &result);
    void logMessage(const QString &level, const QString &message);
    void nodeActivated(const QString &id, const QString &title);
    void cycleFinished();

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
    void beginCycle();
    void executeNext();
    void finishCycle();
    void fault(const QString &message);
};

} // namespace ivs
