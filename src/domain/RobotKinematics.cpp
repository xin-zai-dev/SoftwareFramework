#include "RobotKinematics.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace ivs {
namespace {
constexpr double Pi = 3.14159265358979323846;
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
void clearError(QString *error) {
    if (error)
        error->clear();
}
double normalized(double degrees) {
    double value = std::fmod(degrees, 360.0);
    if (value >= 180)
        value -= 360;
    if (value < -180)
        value += 360;
    return value;
}
bool exactKeys(const QJsonObject &object, const QSet<QString> &keys) {
    const auto actual = object.keys();
    return QSet<QString>(actual.begin(), actual.end()) == keys;
}
} // namespace

bool validateRobotJoints(const RobotJoints &joints, QString *error) {
    clearError(error);
    if (!std::isfinite(joints.j1) || !std::isfinite(joints.j2) || !std::isfinite(joints.z) ||
        !std::isfinite(joints.j4))
        return fail(error, QStringLiteral("Robot joint values must be finite numbers"));
    if (joints.j1 < -170 || joints.j1 > 170 || joints.j2 < -170 || joints.j2 > 170)
        return fail(error, QStringLiteral("Robot J1 and J2 must be in -170..170 degrees"));
    if (joints.z < 0 || joints.z > 150)
        return fail(error, QStringLiteral("Robot Z must be in 0..150 mm"));
    if (joints.j4 < -180 || joints.j4 > 180)
        return fail(error, QStringLiteral("Robot J4 must be in -180..180 degrees"));
    return true;
}

RobotPose robotForward(const RobotJoints &joints) {
    const double a = joints.j1 * Pi / 180;
    const double b = (joints.j1 + joints.j2) * Pi / 180;
    return {RobotLink1 * std::cos(a) + RobotLink2 * std::cos(b),
            RobotLink1 * std::sin(a) + RobotLink2 * std::sin(b), joints.z,
            normalized(joints.j1 + joints.j2 + joints.j4)};
}

bool robotInverse(const RobotPose &pose, bool elbowPositive, RobotJoints *joints, QString *error) {
    clearError(error);
    if (!joints)
        return fail(error, QStringLiteral("Robot joint output is required"));
    if (!std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.z) || !std::isfinite(pose.r))
        return fail(error, QStringLiteral("Robot pose values must be finite numbers"));
    const double measuredRadius = std::hypot(pose.x, pose.y);
    constexpr double RadialTolerance = 1e-9; // mm; accommodates FK roundoff at full extension
    constexpr double OuterRadius = RobotLink1 + RobotLink2;
    constexpr double InnerRadius = RobotLink1 - RobotLink2;
    if (measuredRadius > OuterRadius + RadialTolerance || measuredRadius < InnerRadius - RadialTolerance)
        return fail(error, QStringLiteral("Robot XY target is outside the reachable annulus"));
    const double radius = std::clamp(measuredRadius, InnerRadius, OuterRadius);
    const double cosine = std::clamp((radius * radius - RobotLink1 * RobotLink1 - RobotLink2 * RobotLink2) /
                                         (2 * RobotLink1 * RobotLink2),
                                     -1.0, 1.0);
    const double elbow = std::acos(cosine) * (elbowPositive ? 1 : -1);
    const double shoulder =
        std::atan2(pose.y, pose.x) -
        std::atan2(RobotLink2 * std::sin(elbow), RobotLink1 + RobotLink2 * std::cos(elbow));
    RobotJoints result{normalized(shoulder * 180 / Pi), elbow * 180 / Pi, pose.z, 0};
    // Analytic reconstruction can land a few ulps beyond a legal joint limit.
    // Snap only the solver result; validation of user joint input remains exact.
    for (double *angle : {&result.j1, &result.j2})
        if (std::abs(std::abs(*angle) - 170) < 1e-9)
            *angle = std::copysign(170.0, *angle);
    result.j4 = normalized(normalized(pose.r) - result.j1 - result.j2);
    if (!validateRobotJoints(result, error))
        return false;
    *joints = result;
    return true;
}

QJsonObject robotTeachingToJson(const QVector<RobotWaypoint> &points) {
    QJsonArray array;
    for (const auto &point : points)
        array.append(
            QJsonObject{{QStringLiteral("name"), point.name},
                        {QStringLiteral("joints"), QJsonObject{{QStringLiteral("j1"), point.joints.j1},
                                                               {QStringLiteral("j2"), point.joints.j2},
                                                               {QStringLiteral("z"), point.joints.z},
                                                               {QStringLiteral("j4"), point.joints.j4}}},
                        {QStringLiteral("gripper"), point.gripperClosed}});
    return {{QStringLiteral("schema"), 1}, {QStringLiteral("points"), array}};
}

bool robotTeachingFromJson(const QJsonObject &json, QVector<RobotWaypoint> *points, QString *error) {
    clearError(error);
    if (!points)
        return fail(error, QStringLiteral("Robot teaching output is required"));
    if (!exactKeys(json, {QStringLiteral("schema"), QStringLiteral("points")}) ||
        !json.value(QStringLiteral("schema")).isDouble() ||
        json.value(QStringLiteral("schema")).toDouble() != 1 ||
        !json.value(QStringLiteral("points")).isArray())
        return fail(error, QStringLiteral("Robot teaching requires schema 1 and a points array"));
    const auto array = json.value(QStringLiteral("points")).toArray();
    if (array.size() > 100)
        return fail(error, QStringLiteral("Robot teaching is limited to 100 points"));
    QVector<RobotWaypoint> result;
    result.reserve(array.size());
    for (const auto &value : array) {
        if (!value.isObject())
            return fail(error, QStringLiteral("Every robot teaching point must be an object"));
        const auto point = value.toObject();
        if (!exactKeys(point,
                       {QStringLiteral("name"), QStringLiteral("joints"), QStringLiteral("gripper")}) ||
            !point.value(QStringLiteral("name")).isString() ||
            point.value(QStringLiteral("name")).toString().trimmed().isEmpty() ||
            !point.value(QStringLiteral("joints")).isObject() ||
            !point.value(QStringLiteral("gripper")).isBool())
            return fail(error, QStringLiteral(
                                   "Robot teaching point requires a name, joint object and boolean gripper"));
        const auto numeric = point.value(QStringLiteral("joints")).toObject();
        if (!exactKeys(numeric, {QStringLiteral("j1"), QStringLiteral("j2"), QStringLiteral("z"),
                                 QStringLiteral("j4")}))
            return fail(error, QStringLiteral("Robot teaching joints require exactly j1, j2, z and j4"));
        for (const auto &number : numeric)
            if (!number.isDouble())
                return fail(error, QStringLiteral("Robot teaching joints must be numbers"));
        RobotJoints joints{
            numeric.value(QStringLiteral("j1")).toDouble(), numeric.value(QStringLiteral("j2")).toDouble(),
            numeric.value(QStringLiteral("z")).toDouble(), numeric.value(QStringLiteral("j4")).toDouble()};
        if (!validateRobotJoints(joints, error))
            return false;
        result.append({point.value(QStringLiteral("name")).toString(), joints,
                       point.value(QStringLiteral("gripper")).toBool()});
    }
    *points = std::move(result);
    return true;
}
} // namespace ivs
