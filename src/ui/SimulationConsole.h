#pragma once

#include "domain/Types.h"
#include <QWidget>
#include <memory>

namespace ivs {
class SimulationPlant;

class SimulationConsole : public QWidget {
    Q_OBJECT
  public:
    explicit SimulationConsole(std::shared_ptr<SimulationPlant> plant, QWidget *parent = nullptr);
    ~SimulationConsole() override;
    void setDevices(const QList<DeviceConfig> &devices);
    void setRunning(bool running);
    void resetEdits();
  public slots:
    void refresh();
  signals:
    void message(const QString &level, const QString &text);

  private:
    struct Ui;
    std::unique_ptr<Ui> d;
    void report(bool success, const QString &error, const QString &successText = {});
    void rebuildSelectors();
    void writeLight(int channel);
    void writeSignal(int row, const QString &table, int value);
};
} // namespace ivs
