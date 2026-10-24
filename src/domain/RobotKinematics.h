#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace ivs {

// Generic four-axis SCARA: two planar revolute joints, vertical prismatic Z,
// and a wrist. Millimetres and degrees; no vendor SDK dependency.
struct RobotJoints {
    double j1 = 0;
    double j2 = 90;
    double z = 100;
    double j4 = -90;
};
struct RobotPose {
    double x = 180;
    double y = 140;
    double z = 100;
    double r = 0;
};
struct RobotWaypoint {
    QString name;
    RobotJoints joints;
    bool gripperClosed = false;
};
inline constexpr double RobotLink1 = 180;
inline constexpr double RobotLink2 = 140;
bool validateRobotJoints(const RobotJoints &joints, QString *error = nullptr);
RobotPose robotForward(const RobotJoints &joints);
bool robotInverse(const RobotPose &pose, bool elbowPositive, RobotJoints *joints, QString *error = nullptr);
QJsonObject robotTeachingToJson(const QVector<RobotWaypoint> &points);
bool robotTeachingFromJson(const QJsonObject &json, QVector<RobotWaypoint> *points, QString *error = nullptr);

} // namespace ivs
