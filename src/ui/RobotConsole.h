#pragma once
#include "domain/Types.h"
#include <QWidget>
#include <memory>

namespace ivs {
class SimulationPlant;
class RobotConsole : public QWidget {
    Q_OBJECT
  public:
    RobotConsole(std::shared_ptr<SimulationPlant> plant, const QString &workspace, QWidget *parent = nullptr);
    ~RobotConsole() override;
    void setDevices(const QList<DeviceConfig> &devices);
    void setRunning(bool running);
    void resetEdits();
    bool saveTeaching(const QString &path, QString *error = nullptr) const;
    bool loadTeaching(const QString &path, QString *error = nullptr);
  public slots:
    void refresh();
  signals:
    void message(const QString &level, const QString &text);
    void addRobotRequested();

  private:
    struct Ui;
    std::unique_ptr<Ui> d;
};
} // namespace ivs
