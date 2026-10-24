#pragma once

#include "infrastructure/SimulationPlant.h"
#include <QPointF>
#include <QWidget>

namespace ivs {

// Read-only view of the shared plant. Floor clicks edit a draft target; they do
// not command motion until the operator presses Move in RobotConsole.
class RobotScene final : public QWidget {
    Q_OBJECT
  public:
    explicit RobotScene(QWidget *parent = nullptr);
    void setState(const SimRobotState &state, bool online, bool emergencyStop);
    void clearState();
    void setTarget(const RobotPose &target, bool elbowPositive = true);
    void setTopView(bool topView);
    QPointF project(const RobotPose &pose) const;
    QPointF floorPosition(const QPointF &point) const;

  signals:
    void targetRequested(double x, double y);

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

  private:
    double sceneScale() const;
    QPointF sceneOrigin() const;
    SimRobotState m_state;
    RobotPose m_target;
    bool m_hasState = false;
    bool m_online = false;
    bool m_emergencyStop = false;
    bool m_topView = false;
    bool m_targetElbowPositive = true;
};

} // namespace ivs
