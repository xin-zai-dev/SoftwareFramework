#include "ui/RobotScene.h"
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace ivs {
namespace {
const QColor Navy(31, 57, 86);
const QColor Blue(47, 104, 166);
const QColor Teal(16, 154, 165);
const QColor Orange(241, 158, 65);
QPointF planarElbow(const RobotJoints &joints) {
    const double angle = qDegreesToRadians(joints.j1);
    return {RobotLink1 * std::cos(angle), RobotLink1 * std::sin(angle)};
}
} // namespace

RobotScene::RobotScene(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("robotScene"));
    setMinimumSize(480, 360);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setCursor(Qt::CrossCursor);
    setToolTip(tr("点击底板设置 XY 目标；Z / R 保持不变，按「移动到 XYZ」执行。"));
}

void RobotScene::setState(const SimRobotState &state, bool online, bool emergencyStop) {
    m_state = state;
    m_hasState = true;
    m_online = online;
    m_emergencyStop = emergencyStop;
    update();
}
void RobotScene::clearState() {
    m_hasState = false;
    update();
}
void RobotScene::setTarget(const RobotPose &target, bool elbowPositive) {
    m_target = target;
    m_targetElbowPositive = elbowPositive;
    update();
}
void RobotScene::setTopView(bool topView) {
    m_topView = topView;
    update();
}
double RobotScene::sceneScale() const {
    return m_topView ? std::min((width() - 60.0) / 720, (height() - 125.0) / 720)
                     : std::min((width() - 55.0) / 1050, (height() - 115.0) / 650);
}
QPointF RobotScene::sceneOrigin() const {
    return {width() * 0.49, height() * (m_topView ? 0.56 : 0.57)};
}
QPointF RobotScene::project(const RobotPose &pose) const {
    const double s = sceneScale();
    return sceneOrigin() +
           (m_topView ? QPointF(pose.x * s, -pose.y * s)
                      : QPointF(0.86 * (pose.x - pose.y) * s, (0.43 * (pose.x + pose.y) - pose.z) * s));
}
QPointF RobotScene::floorPosition(const QPointF &point) const {
    const QPointF relative = (point - sceneOrigin()) / sceneScale();
    if (m_topView)
        return {relative.x(), -relative.y()};
    return {(relative.x() / 0.86 + relative.y() / 0.43) / 2, (relative.y() / 0.43 - relative.x() / 0.86) / 2};
}

void RobotScene::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && m_hasState && isEnabled()) {
        const QPointF xy = floorPosition(event->position());
        if (std::abs(xy.x()) <= 350 && std::abs(xy.y()) <= 350)
            emit targetRequested(xy.x(), xy.y());
    }
    QWidget::mousePressEvent(event);
}

void RobotScene::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const QRectF bounds = rect().adjusted(1, 1, -1, -1);
    QLinearGradient background(0, 0, 0, height());
    background.setColorAt(0, QColor("#fbfdff"));
    background.setColorAt(1, QColor("#edf3f9"));
    p.setBrush(background);
    p.setPen(QColor("#dfe7f1"));
    p.drawRoundedRect(bounds, 12, 12);
    p.setPen(Navy);
    QFont title = font();
    title.setPixelSize(15);
    title.setBold(true);
    p.setFont(title);
    p.drawText(QRectF(20, 16, width() - 40, 24), tr("SCARA · 四轴工作站"));
    QFont small = font();
    small.setPixelSize(11);
    p.setFont(small);
    p.setPen(QColor("#70839b"));
    p.drawText(QRectF(20, 42, width() - 40, 20),
               tr("180 + 140 mm  /  Z 0–150 mm  /  %1").arg(m_topView ? tr("俯视") : tr("等距视图")));

    auto at = [this](double x, double y, double z = 0) { return project({x, y, z, 0}); };
    auto worldLine = [&](double x1, double y1, double x2, double y2) { p.drawLine(at(x1, y1), at(x2, y2)); };
    auto floorCircle = [&](double radius) {
        QPainterPath path;
        for (int i = 0; i <= 96; ++i) {
            const double angle = i * 2 * M_PI / 96;
            const QPointF point = at(radius * std::cos(angle), radius * std::sin(angle));
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        return path;
    };
    const QPolygonF floor{at(-350, -350), at(350, -350), at(350, 350), at(-350, 350)};
    p.setBrush(QColor(244, 248, 252));
    p.setPen(QColor("#dce5ef"));
    p.drawPolygon(floor);
    p.setPen(QPen(QColor("#dfe7f0"), 0.8));
    for (int value = -300; value <= 300; value += 50) {
        worldLine(value, -350, value, 350);
        worldLine(-350, value, 350, value);
    }
    p.setBrush(QColor(74, 135, 191, 9));
    p.setPen(QPen(QColor(103, 150, 189, 100), 1.2, Qt::DashLine));
    p.drawPath(floorCircle(320));
    p.setBrush(Qt::NoBrush);
    p.drawPath(floorCircle(40));
    p.setPen(QPen(QColor("#9eb4ca"), 1.3));
    worldLine(-345, 0, 345, 0);
    worldLine(0, -345, 0, 345);
    p.setPen(QColor("#849bb2"));
    p.drawText(at(345, 0) + QPointF(4, 2), QStringLiteral("X"));
    p.drawText(at(0, 345) + QPointF(-8, -4), QStringLiteral("Y"));

    auto station = [&](double x, double y, const QColor &color, const QString &label, const QString &detail) {
        const QPolygonF rim{at(x - 34, y - 34, 5), at(x + 34, y - 34, 5), at(x + 34, y + 34, 5),
                            at(x - 34, y + 34, 5)};
        p.setPen(QPen(color.darker(105), 1.4));
        p.setBrush(QColor(color.red(), color.green(), color.blue(), 30));
        p.drawPolygon(rim);
        p.setPen(QPen(QColor(color.red(), color.green(), color.blue(), 120), 4));
        p.drawLine(at(x - 26, y - 26, 8), at(x + 26, y - 26, 8));
        const QPointF labelAt = at(x, y) + QPointF(0, 26);
        p.setPen(color.darker(140));
        p.drawText(QRectF(labelAt.x() - 65, labelAt.y(), 130, 17), Qt::AlignHCenter, label);
        p.setPen(QColor("#7b8da0"));
        p.drawText(QRectF(labelAt.x() - 80, labelAt.y() + 17, 160, 16), Qt::AlignHCenter, detail);
    };
    station(220, 40, Orange, tr("上料位"), QStringLiteral("220, 40, 20"));
    station(80, 200, QColor("#29a77b"), tr("OK 料箱 · %1").arg(m_state.okCount),
            QStringLiteral("80, 200, 20"));
    station(80, -200, QColor("#d97075"), tr("NG 料箱 · %1").arg(m_state.ngCount),
            QStringLiteral("80, −200, 20"));

    if (!m_hasState) {
        p.setPen(Navy);
        p.drawText(rect().adjusted(20, 70, -20, -20), Qt::AlignCenter, tr("添加仿真机械臂，开始定位与分拣"));
        return;
    }
    if (m_state.trail.size() > 1) {
        QPainterPath trail;
        trail.moveTo(project(m_state.trail.first()));
        for (int i = 1; i < m_state.trail.size(); ++i)
            trail.lineTo(project(m_state.trail.at(i)));
        p.setPen(QPen(QColor(16, 154, 165, 145), 1.6, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath(trail);
    }
    const QPointF target = project(m_target);
    p.setPen(QPen(QColor(16, 154, 165, 100), 1, Qt::DashLine));
    p.drawLine(target, at(m_target.x, m_target.y));
    p.setPen(QPen(Teal, 1.7));
    p.setBrush(QColor(16, 154, 165, 10));
    p.drawEllipse(target, 8, 8);
    p.drawLine(target + QPointF(-12, 0), target + QPointF(12, 0));
    p.drawLine(target + QPointF(0, -12), target + QPointF(0, 12));
    p.drawText(target + QPointF(12, -10), tr("目标"));
    RobotJoints ghost;
    if (robotInverse(m_target, m_targetElbowPositive, &ghost)) {
        const QPointF elbow = planarElbow(ghost);
        p.setPen(QPen(QColor(16, 154, 165, 75), 8 * sceneScale() + 2, Qt::DashLine, Qt::RoundCap));
        p.drawLine(at(0, 0, 180), at(elbow.x(), elbow.y(), 180));
        p.drawLine(at(elbow.x(), elbow.y(), 180), at(m_target.x, m_target.y, 180));
    }

    const double s = sceneScale();
    const QPointF elbow = planarElbow(m_state.joints);
    const RobotPose tcp = m_state.tcp;
    const QPointF shoulder = at(0, 0, 180);
    const QPointF joint = at(elbow.x(), elbow.y(), 180);
    const QPointF wrist = at(tcp.x, tcp.y, 180);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(31, 57, 86, 12));
    p.drawEllipse(at(0, 0) + QPointF(0, 5), 46 * s, 20 * s);
    p.setPen(QPen(QColor("#cad6e3"), 32 * s, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(at(0, 0, 18), shoulder);
    p.setPen(QPen(QColor("#edf3f8"), 19 * s, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(at(-4, -4, 26), shoulder + QPointF(-2 * s, 0));
    p.setPen(QPen(Navy, 48 * s, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(at(-8, -8, 10), at(8, 8, 10));
    auto link = [&](const QPointF &a, const QPointF &b, int width, const QColor &color) {
        p.setPen(QPen(QColor(25, 51, 82, 35), (width + 4) * s, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a + QPointF(0, 6 * s), b + QPointF(0, 6 * s));
        p.setPen(QPen(color, width * s, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
        p.setPen(QPen(color.lighter(125), 5 * s, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a + QPointF(0, -4 * s), b + QPointF(0, -4 * s));
    };
    link(shoulder, joint, 28, Blue);
    link(joint, wrist, 23, QColor("#548bb9"));
    for (const auto &point : {shoulder, joint, wrist}) {
        p.setPen(QPen(QColor("#f0f5fa"), 3 * s));
        p.setBrush(Navy);
        p.drawEllipse(point, 13 * s, 13 * s);
        p.setBrush(QColor("#adc3d8"));
        p.setPen(Qt::NoPen);
        p.drawEllipse(point, 4 * s, 4 * s);
    }
    p.setPen(QPen(QColor("#74869c"), 7 * s, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(at(tcp.x, tcp.y, 205), project(tcp));
    p.setPen(QPen(QColor("#dce5ed"), 2 * s));
    p.drawLine(at(tcp.x - 3, tcp.y - 3, 205), project({tcp.x - 3, tcp.y - 3, tcp.z, 0}));
    const double r = qDegreesToRadians(tcp.r);
    const double jaw = m_state.gripperClosed ? 5 : 15;
    const double dx = std::cos(r), dy = std::sin(r);
    p.setPen(QPen(Navy, 4 * s + 1, Qt::SolidLine, Qt::RoundCap));
    for (double side : {-1.0, 1.0}) {
        const double x = tcp.x - dy * jaw * side;
        const double y = tcp.y + dx * jaw * side;
        p.drawLine(at(x - dx * 9, y - dy * 9, tcp.z), at(x + dx * 9, y + dy * 9, tcp.z));
    }
    p.setPen(QPen(Orange, 2));
    p.drawLine(project(tcp), at(tcp.x + dx * 30, tcp.y + dy * 30, tcp.z));
    p.setBrush(Orange);
    p.setPen(Qt::NoPen);
    p.drawEllipse(project(tcp), 3, 3);

    if (!m_state.partLocation.isEmpty()) {
        const RobotPose part = m_state.carryingPart ? tcp : m_state.partPose;
        const QPointF center = project(part);
        p.setBrush(QColor(39, 55, 74, 25));
        p.drawEllipse(at(part.x, part.y), 13 * s, 7 * s);
        p.setPen(QPen(Orange.darker(120), 1.2));
        p.setBrush(Orange.lighter(115));
        p.drawRoundedRect(QRectF(center.x() - 10 * s, center.y() - 8 * s, 20 * s, 16 * s), 3, 3);
        p.setPen(Orange.darker(130));
        p.drawLine(center + QPointF(-5 * s, 0), center + QPointF(5 * s, 0));
    }

    const QString badge = m_emergencyStop          ? tr("急停")
                          : !m_online              ? tr("离线")
                          : m_state.programRunning ? tr("程序运行中")
                          : m_state.moving         ? tr("运动中")
                                                   : tr("虚拟工作站");
    const QColor badgeColor = m_emergencyStop || !m_online ? QColor("#c64b58") : Teal;
    p.setBrush(QColor(badgeColor.red(), badgeColor.green(), badgeColor.blue(), 15));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(width() - 125, 20, 105, 25), 12, 12);
    p.setPen(badgeColor);
    p.drawText(QRectF(width() - 125, 20, 105, 25), Qt::AlignCenter, badge);
    p.setPen(QColor("#70839b"));
    p.drawText(QRectF(18, height() - 44, width() - 36, 17),
               tr("点击底板设置 XY 目标  ·  青色虚线为轨迹  ·  橙色为工件 / TCP"));
    p.setPen(QColor("#8da0b5"));
    p.drawText(QRectF(18, height() - 25, width() - 36, 15), tr("单位 mm / °  ·  关节空间插补  ·  虚拟抓放"));
}

} // namespace ivs
