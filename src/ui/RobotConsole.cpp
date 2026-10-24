#include "ui/RobotConsole.h"
#include "infrastructure/SimulationPlant.h"
#include "ui/RobotScene.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <array>
#include <functional>
#include <limits>

namespace ivs {
namespace {
const SimDeviceState *selectedRobot(const SimulationSnapshot &snapshot, const QString &id) {
    for (const auto &device : snapshot.devices)
        if (device.id == id && device.category == QStringLiteral("robot"))
            return &device;
    return nullptr;
}
QPushButton *button(const QString &text, const char *name, QWidget *parent) {
    auto *result = new QPushButton(text, parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setMinimumHeight(30);
    return result;
}
QDoubleSpinBox *number(const char *name, double minimum, double maximum, const QString &suffix,
                       QWidget *parent) {
    auto *result = new QDoubleSpinBox(parent);
    result->setObjectName(QString::fromLatin1(name));
    result->setRange(minimum, maximum);
    result->setDecimals(1);
    result->setSingleStep(1);
    result->setSuffix(suffix);
    result->setKeyboardTracking(false);
    result->setMinimumWidth(86);
    return result;
}
void checked(QCheckBox *box, bool value) {
    const QSignalBlocker blocker(box);
    box->setChecked(value);
}
} // namespace

struct RobotConsole::Ui {
    struct Joint {
        QDoubleSpinBox *target = nullptr;
        QSlider *slider = nullptr;
        QLabel *actual = nullptr;
        QPushButton *minus = nullptr;
        QPushButton *plus = nullptr;
    };
    RobotConsole *owner = nullptr;
    std::shared_ptr<SimulationPlant> plant;
    QString workspace;
    QString boundId;
    quint64 generation = std::numeric_limits<quint64>::max();
    bool running = false;
    bool refreshing = false;
    bool forceEdits = true;
    bool manual = false;
    QComboBox *device = nullptr;
    QComboBox *elbow = nullptr;
    QCheckBox *online = nullptr;
    QCheckBox *enabled = nullptr;
    QLabel *state = nullptr;
    QLabel *tcp = nullptr;
    QLabel *notice = nullptr;
    QLabel *part = nullptr;
    QLabel *teachingHint = nullptr;
    QPushButton *add = nullptr;
    QPushButton *moveJoints = nullptr;
    QPushButton *moveTcp = nullptr;
    QPushButton *home = nullptr;
    QPushButton *stop = nullptr;
    QPushButton *emergency = nullptr;
    QPushButton *open = nullptr;
    QPushButton *close = nullptr;
    QPushButton *restock = nullptr;
    QPushButton *sortOk = nullptr;
    QPushButton *sortNg = nullptr;
    QPushButton *teach = nullptr;
    QPushButton *remove = nullptr;
    QPushButton *movePoint = nullptr;
    QPushButton *play = nullptr;
    QPushButton *save = nullptr;
    QPushButton *load = nullptr;
    QLineEdit *name = nullptr;
    QTableWidget *table = nullptr;
    QSpinBox *speed = nullptr;
    QSlider *speedSlider = nullptr;
    RobotScene *scene = nullptr;
    QWidget *controls = nullptr;
    std::array<Joint, 4> joints;
    std::array<QDoubleSpinBox *, 4> pose{};
    QVector<RobotWaypoint> points;
    QJsonObject shownTeaching;

    QString id() const {
        return device->currentData().toString();
    }
    RobotPose targetPose() const {
        return {pose[0]->value(), pose[1]->value(), pose[2]->value(), pose[3]->value()};
    }
    RobotJoints targetJoints() const {
        return {joints[0].target->value(), joints[1].target->value(), joints[2].target->value(),
                joints[3].target->value()};
    }
    void report(bool success, const QString &error, const QString &successText = {}) {
        const QString text = success ? successText : (error.isEmpty() ? owner->tr("操作失败。") : error);
        if (!text.isEmpty()) {
            notice->setText(text);
            notice->setStyleSheet(success ? QStringLiteral("color:#287763;")
                                          : QStringLiteral("color:#bd4254;font-weight:600;"));
            emit owner->message(success ? QStringLiteral("INFO") : QStringLiteral("ERROR"), text);
        }
        owner->refresh();
    }
    void command(const std::function<bool(QString *)> &action, const QString &success = {}) {
        if (!plant || refreshing)
            return;
        QString error;
        const bool ok = action(&error);
        report(ok, error, success);
    }
};

RobotConsole::RobotConsole(std::shared_ptr<SimulationPlant> plant, const QString &workspace, QWidget *parent)
    : QWidget(parent), d(std::make_unique<Ui>()) {
    d->owner = this;
    d->plant = std::move(plant);
    d->workspace = workspace;
    setObjectName(QStringLiteral("robotConsole"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 8, 24, 16);
    layout->setSpacing(9);
    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(tr("机械臂实验室"), this);
    title->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:#173655;"));
    heading->addWidget(title);
    heading->addStretch();
    d->add = button(tr("添加仿真机械臂"), "robotAddDeviceButton", this);
    heading->addWidget(d->add);
    layout->addLayout(heading);
    auto *intro = new QLabel(tr("四轴 SCARA · 定位、夹爪、示教与视觉分拣，共享同一虚拟工作站。"), this);
    intro->setStyleSheet(QStringLiteral("color:#77849a;"));
    layout->addWidget(intro);
    d->notice =
        new QLabel(tr("点击底板设置 XY，再按「移动到 XYZ」。可从上料位抓取工件，送入 OK / NG 料箱。"), this);
    d->notice->setObjectName(QStringLiteral("robotMessage"));
    d->notice->setStyleSheet(QStringLiteral("color:#53708c;"));
    d->notice->setWordWrap(true);
    d->notice->setMinimumHeight(24);
    layout->addWidget(d->notice);

    auto *connection = new QGroupBox(tr("工作站"), this);
    auto *connectionLayout = new QHBoxLayout(connection);
    connectionLayout->setContentsMargins(12, 10, 12, 10);
    d->device = new QComboBox(connection);
    d->device->setObjectName(QStringLiteral("robotDeviceCombo"));
    d->device->setMinimumWidth(175);
    connectionLayout->addWidget(d->device, 1);
    d->online = new QCheckBox(tr("在线"), connection);
    d->online->setObjectName(QStringLiteral("robotOnlineCheck"));
    d->online->setToolTip(tr("断开连接会取消当前动作和剩余程序。"));
    connectionLayout->addWidget(d->online);
    d->enabled = new QCheckBox(tr("使能"), connection);
    d->enabled->setObjectName(QStringLiteral("robotEnabledCheck"));
    connectionLayout->addWidget(d->enabled);
    d->state = new QLabel(tr("暂无设备"), connection);
    d->state->setObjectName(QStringLiteral("robotStateLabel"));
    d->state->setWordWrap(true);
    connectionLayout->addWidget(d->state, 1);
    d->stop = button(tr("停止"), "robotStopButton", connection);
    d->stop->setStyleSheet(QStringLiteral("color:#bd4254;border:1px solid #e8c7cc;"));
    connectionLayout->addWidget(d->stop);
    d->emergency = button(tr("虚拟急停"), "robotEmergencyButton", connection);
    d->emergency->setStyleSheet(QStringLiteral("background:#c64d5d;color:white;border:0;font-weight:600;"));
    connectionLayout->addWidget(d->emergency);
    layout->addWidget(connection);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setChildrenCollapsible(false);
    auto *scenePanel = new QWidget(splitter);
    auto *sceneLayout = new QVBoxLayout(scenePanel);
    sceneLayout->setContentsMargins(0, 0, 0, 0);
    sceneLayout->setSpacing(8);
    auto *sceneHeading = new QHBoxLayout;
    d->tcp = new QLabel(scenePanel);
    d->tcp->setObjectName(QStringLiteral("robotTcpLabel"));
    d->tcp->setStyleSheet(QStringLiteral("font-weight:600;color:#314e6d;padding:4px 0;"));
    sceneHeading->addWidget(d->tcp, 1);
    auto *view = new QComboBox(scenePanel);
    view->setObjectName(QStringLiteral("robotViewCombo"));
    view->addItems({tr("等距视图"), tr("俯视图")});
    sceneHeading->addWidget(view);
    sceneLayout->addLayout(sceneHeading);
    d->scene = new RobotScene(scenePanel);
    sceneLayout->addWidget(d->scene, 1);
    d->part = new QLabel(scenePanel);
    d->part->setObjectName(QStringLiteral("robotPartLabel"));
    d->part->setWordWrap(true);
    d->part->setStyleSheet(QStringLiteral("color:#53708c;padding:5px 0;"));
    sceneLayout->addWidget(d->part);
    splitter->addWidget(scenePanel);

    auto *tabs = new QTabWidget(splitter);
    tabs->setObjectName(QStringLiteral("robotControlTabs"));
    tabs->setMinimumWidth(345);
    tabs->setMaximumWidth(475);
    tabs->setStyleSheet(
        QStringLiteral("QTabWidget::pane{border:1px solid #e1e6ef;border-radius:8px;background:white;}"
                       "QTabBar::tab{padding:10px 22px;color:#667d95;background:#edf2f7;}"
                       "QTabBar::tab:selected{background:white;color:#254e79;font-weight:600;}"));
    auto *motionScroll = new QScrollArea(tabs);
    motionScroll->setWidgetResizable(true);
    motionScroll->setFrameShape(QFrame::NoFrame);
    d->controls = new QWidget(motionScroll);
    auto *controls = new QVBoxLayout(d->controls);
    controls->setContentsMargins(12, 10, 12, 12);
    controls->setSpacing(10);
    motionScroll->setWidget(d->controls);
    tabs->addTab(motionScroll, tr("手动控制"));

    auto *speedRow = new QHBoxLayout;
    speedRow->addWidget(new QLabel(tr("速度"), d->controls));
    d->speedSlider = new QSlider(Qt::Horizontal, d->controls);
    d->speedSlider->setRange(1, 100);
    d->speedSlider->setObjectName(QStringLiteral("robotSpeedSlider"));
    speedRow->addWidget(d->speedSlider, 1);
    d->speed = new QSpinBox(d->controls);
    d->speed->setObjectName(QStringLiteral("robotSpeed"));
    d->speed->setRange(1, 100);
    d->speed->setSuffix(QStringLiteral(" %"));
    speedRow->addWidget(d->speed);
    controls->addLayout(speedRow);

    auto *poseCard = new QGroupBox(tr("XYZ / R 终点定位"), d->controls);
    auto *poseGrid = new QGridLayout(poseCard);
    const std::array<const char *, 4> poseNames{"robotXTarget", "robotYTarget", "robotZTarget",
                                                "robotRTarget"};
    const QStringList poseLabels{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"),
                                 QStringLiteral("R")};
    for (int i = 0; i < 4; ++i) {
        const bool rotation = i == 3;
        const double low = i == 2 ? 0 : rotation ? -180 : -350;
        const double high = i == 2 ? 150 : rotation ? 180 : 350;
        d->pose[i] = number(poseNames[i], low, high, rotation ? QStringLiteral(" °") : QStringLiteral(" mm"),
                            poseCard);
        poseGrid->addWidget(new QLabel(poseLabels.at(i), poseCard), i / 2, (i % 2) * 2);
        poseGrid->addWidget(d->pose[i], i / 2, (i % 2) * 2 + 1);
        connect(d->pose[i], &QDoubleSpinBox::valueChanged, this, [this] {
            if (!d->refreshing)
                refresh();
        });
    }
    d->elbow = new QComboBox(poseCard);
    d->elbow->setObjectName(QStringLiteral("robotElbowCombo"));
    d->elbow->addItem(tr("正肘解 · J2 ≥ 0"), true);
    d->elbow->addItem(tr("负肘解 · J2 ≤ 0"), false);
    poseGrid->addWidget(d->elbow, 2, 0, 1, 4);
    d->moveTcp = button(tr("移动到 XYZ"), "robotMoveTcp", poseCard);
    d->moveTcp->setStyleSheet(
        QStringLiteral("background:#2865a2;color:white;border-color:#2865a2;font-weight:600;"));
    poseGrid->addWidget(d->moveTcp, 3, 0, 1, 4);
    controls->addWidget(poseCard);

    auto *jointCard = new QGroupBox(tr("关节目标 / 点动 ±1"), d->controls);
    auto *jointGrid = new QGridLayout(jointCard);
    jointGrid->setHorizontalSpacing(5);
    const std::array<const char *, 4> jointNames{"robotJ1Target", "robotJ2Target", "robotJointZTarget",
                                                 "robotJ4Target"};
    const QStringList jointLabels{QStringLiteral("J1"), QStringLiteral("J2"), QStringLiteral("Z"),
                                  QStringLiteral("J4")};
    for (int i = 0; i < 4; ++i) {
        auto &joint = d->joints[i];
        const int maximum = i == 2 ? 150 : i == 3 ? 180 : 170;
        const int minimum = i == 2 ? 0 : -maximum;
        joint.target = number(jointNames[i], minimum, maximum,
                              i == 2 ? QStringLiteral(" mm") : QStringLiteral(" °"), jointCard);
        joint.actual = new QLabel(jointCard);
        joint.actual->setObjectName(QStringLiteral("robotJoint%1Actual").arg(i));
        joint.actual->setMinimumWidth(45);
        joint.actual->setStyleSheet(QStringLiteral("color:#667d95;"));
        jointGrid->addWidget(new QLabel(jointLabels.at(i), jointCard), i * 2, 0);
        jointGrid->addWidget(joint.target, i * 2, 1, 1, 2);
        jointGrid->addWidget(joint.actual, i * 2, 3);
        joint.minus =
            button(QStringLiteral("−"), qPrintable(QStringLiteral("robotJoint%1Minus").arg(i)), jointCard);
        joint.plus =
            button(QStringLiteral("+"), qPrintable(QStringLiteral("robotJoint%1Plus").arg(i)), jointCard);
        joint.minus->setFixedWidth(32);
        joint.plus->setFixedWidth(32);
        joint.minus->setStyleSheet(QStringLiteral("padding:2px;"));
        joint.plus->setStyleSheet(QStringLiteral("padding:2px;"));
        jointGrid->addWidget(joint.minus, i * 2, 4);
        jointGrid->addWidget(joint.plus, i * 2, 5);
        joint.slider = new QSlider(Qt::Horizontal, jointCard);
        joint.slider->setRange(minimum * 10, maximum * 10);
        joint.slider->setObjectName(QStringLiteral("robotJoint%1Slider").arg(i));
        jointGrid->addWidget(joint.slider, i * 2 + 1, 1, 1, 5);
        connect(joint.target, &QDoubleSpinBox::valueChanged, this, [this, i](double value) {
            const QSignalBlocker blocker(d->joints[i].slider);
            d->joints[i].slider->setValue(qRound(value * 10));
        });
        connect(joint.slider, &QSlider::valueChanged, this,
                [this, i](int value) { d->joints[i].target->setValue(value / 10.0); });
        auto jog = [this, i](double step) {
            if (!d->manual)
                return;
            const auto snapshot = d->plant->snapshot();
            const auto *device = selectedRobot(snapshot, d->id());
            if (!device)
                return;
            RobotJoints target = device->robot.joints;
            if (i == 0)
                target.j1 += step;
            else if (i == 1)
                target.j2 += step;
            else if (i == 2)
                target.z += step;
            else
                target.j4 += step;
            d->command([&](QString *error) {
                return d->plant->moveRobotJoints(d->id(), target, d->speed->value(), error);
            });
        };
        connect(joint.minus, &QPushButton::clicked, this, [jog] { jog(-1); });
        connect(joint.plus, &QPushButton::clicked, this, [jog] { jog(1); });
    }
    d->moveJoints = button(tr("应用关节目标"), "robotMoveJoints", jointCard);
    d->home = button(tr("回待机位"), "robotHomeButton", jointCard);
    jointGrid->addWidget(d->moveJoints, 8, 0, 1, 4);
    jointGrid->addWidget(d->home, 8, 4, 1, 2);
    controls->addWidget(jointCard);

    auto *gripCard = new QGroupBox(tr("夹爪与工件"), d->controls);
    auto *gripGrid = new QGridLayout(gripCard);
    d->open = button(tr("张开夹爪"), "robotGripOpen", gripCard);
    d->close = button(tr("闭合夹爪"), "robotGripClose", gripCard);
    d->restock = button(tr("补充工件"), "robotRestockButton", gripCard);
    d->sortOk = button(tr("分拣 → OK"), "robotSortOk", gripCard);
    d->sortNg = button(tr("分拣 → NG"), "robotSortNg", gripCard);
    d->sortOk->setStyleSheet(QStringLiteral("color:#258564;"));
    d->sortNg->setStyleSheet(QStringLiteral("color:#bd5666;"));
    gripGrid->addWidget(d->open, 0, 0);
    gripGrid->addWidget(d->close, 0, 1);
    gripGrid->addWidget(d->restock, 1, 0, 1, 2);
    gripGrid->addWidget(d->sortOk, 2, 0);
    gripGrid->addWidget(d->sortNg, 2, 1);
    controls->addWidget(gripCard);
    controls->addStretch();

    auto *teaching = new QWidget(tabs);
    auto *teachLayout = new QVBoxLayout(teaching);
    teachLayout->setContentsMargins(12, 14, 12, 12);
    auto *teachIntro = new QLabel(tr("记录实际位置与夹爪状态，按顺序回放，最多 100 点。"), teaching);
    teachIntro->setWordWrap(true);
    teachIntro->setStyleSheet(QStringLiteral("color:#667d95;"));
    teachLayout->addWidget(teachIntro);
    auto *nameRow = new QHBoxLayout;
    d->name = new QLineEdit(teaching);
    d->name->setObjectName(QStringLiteral("robotTeachName"));
    d->name->setPlaceholderText(tr("点位名称，如「上料上方」"));
    d->name->setMaxLength(100);
    nameRow->addWidget(d->name, 1);
    d->teach = button(tr("记录当前点"), "robotTeachButton", teaching);
    nameRow->addWidget(d->teach);
    teachLayout->addLayout(nameRow);
    d->table = new QTableWidget(teaching);
    d->table->setObjectName(QStringLiteral("robotTeachTable"));
    d->table->setColumnCount(6);
    d->table->setHorizontalHeaderLabels({tr("名称"), QStringLiteral("J1 °"), QStringLiteral("J2 °"),
                                         QStringLiteral("Z mm"), QStringLiteral("J4 °"), tr("夹爪")});
    d->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    d->table->setSelectionMode(QAbstractItemView::SingleSelection);
    d->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    d->table->setAlternatingRowColors(true);
    d->table->verticalHeader()->setDefaultSectionSize(33);
    d->table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    d->table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    d->table->setMinimumHeight(220);
    teachLayout->addWidget(d->table, 1);
    auto *pointButtons = new QHBoxLayout;
    d->movePoint = button(tr("移动到选中点"), "robotMovePoint", teaching);
    d->remove = button(tr("删除"), "robotRemovePoint", teaching);
    pointButtons->addWidget(d->movePoint, 1);
    pointButtons->addWidget(d->remove);
    teachLayout->addLayout(pointButtons);
    auto *pointHint = new QLabel(tr("单点移动只定位；顺序回放同时还原夹爪。"), teaching);
    pointHint->setWordWrap(true);
    pointHint->setStyleSheet(QStringLiteral("color:#8795a8;font-size:11px;"));
    teachLayout->addWidget(pointHint);
    d->play = button(tr("▶ 顺序回放"), "robotPlayButton", teaching);
    d->play->setStyleSheet(QStringLiteral("background:#2865a2;color:white;border-color:#2865a2;"));
    teachLayout->addWidget(d->play);
    auto *fileButtons = new QHBoxLayout;
    d->save = button(tr("保存 JSON"), "robotSaveTeaching", teaching);
    d->load = button(tr("加载 JSON"), "robotLoadTeaching", teaching);
    fileButtons->addWidget(d->save);
    fileButtons->addWidget(d->load);
    teachLayout->addLayout(fileButtons);
    d->teachingHint = new QLabel(teaching);
    d->teachingHint->setWordWrap(true);
    d->teachingHint->setStyleSheet(QStringLiteral("color:#667d95;"));
    teachLayout->addWidget(d->teachingHint);
    tabs->addTab(teaching, tr("示教点位"));
    splitter->addWidget(tabs);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    splitter->setSizes({700, 410});
    layout->addWidget(splitter, 1);

    connect(d->add, &QPushButton::clicked, this, &RobotConsole::addRobotRequested);
    connect(d->device, &QComboBox::currentIndexChanged, this, [this] { resetEdits(); });
    connect(view, &QComboBox::currentIndexChanged, this,
            [this](int index) { d->scene->setTopView(index == 1); });
    connect(d->elbow, &QComboBox::currentIndexChanged, this, [this] {
        if (!d->refreshing)
            refresh();
    });
    connect(d->scene, &RobotScene::targetRequested, this, [this](double x, double y) {
        if (!d->manual)
            return;
        d->pose[0]->setValue(x);
        d->pose[1]->setValue(y);
        refresh();
    });
    connect(d->speed, &QSpinBox::valueChanged, d->speedSlider, &QSlider::setValue);
    connect(d->speedSlider, &QSlider::valueChanged, d->speed, &QSpinBox::setValue);
    connect(d->online, &QCheckBox::toggled, this, [this](bool online) {
        if (!d->refreshing)
            d->command([&](QString *error) { return d->plant->setConnected(d->id(), online, error); });
    });
    connect(d->enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!d->refreshing)
            d->command([&](QString *error) { return d->plant->enableRobot(d->id(), enabled, error); });
    });
    connect(d->moveJoints, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) {
            return d->plant->moveRobotJoints(d->id(), d->targetJoints(), d->speed->value(), error);
        });
    });
    connect(d->moveTcp, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) {
            return d->plant->moveRobotTcp(d->id(), d->targetPose(), d->speed->value(),
                                          d->elbow->currentData().toBool(), error);
        });
    });
    connect(d->home, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) { return d->plant->homeRobot(d->id(), d->speed->value(), error); });
    });
    connect(d->stop, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) { return d->plant->stopRobot(d->id(), error); },
                   tr("动作已停止；再次启动前请重新发令。"));
    });
    connect(d->emergency, &QPushButton::clicked, this, [this] {
        if (!d->plant)
            return;
        const bool enabled = !d->plant->snapshot().emergencyStop;
        d->plant->setEmergencyStop(enabled);
        d->report(true, {},
                  enabled ? tr("虚拟急停已触发，所有动作与程序已取消。")
                          : tr("虚拟急停已解除；需要手动重新发令。"));
    });
    connect(d->open, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) { return d->plant->setRobotGripper(d->id(), false, error); });
    });
    connect(d->close, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) { return d->plant->setRobotGripper(d->id(), true, error); });
    });
    connect(d->restock, &QPushButton::clicked, this,
            [this] { d->command([&](QString *error) { return d->plant->restockRobot(d->id(), error); }); });
    connect(d->sortOk, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) {
            return d->plant->startRobotSort(d->id(), QStringLiteral("OK"), d->speed->value(), error);
        });
    });
    connect(d->sortNg, &QPushButton::clicked, this, [this] {
        d->command([&](QString *error) {
            return d->plant->startRobotSort(d->id(), QStringLiteral("NG"), d->speed->value(), error);
        });
    });
    connect(d->teach, &QPushButton::clicked, this, [this] {
        const QString name = d->name->text().trimmed().isEmpty() ? tr("点位 %1").arg(d->points.size() + 1)
                                                                 : d->name->text().trimmed();
        d->command([&](QString *error) { return d->plant->teachRobotPoint(d->id(), name, error); });
    });
    connect(d->remove, &QPushButton::clicked, this, [this] {
        const int row = d->table->currentRow();
        if (row >= 0)
            d->command([&](QString *error) { return d->plant->removeRobotPoint(d->id(), row, error); });
    });
    connect(d->movePoint, &QPushButton::clicked, this, [this] {
        const int row = d->table->currentRow();
        if (row >= 0 && row < d->points.size())
            d->command([&](QString *error) {
                return d->plant->moveRobotJoints(d->id(), d->points.at(row).joints, d->speed->value(), error);
            });
    });
    connect(d->play, &QPushButton::clicked, this, [this] {
        d->command(
            [&](QString *error) { return d->plant->playRobotTeaching(d->id(), d->speed->value(), error); });
    });
    connect(d->table, &QTableWidget::itemSelectionChanged, this, [this] {
        if (!d->refreshing)
            refresh();
    });
    connect(d->save, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, tr("保存机械臂示教点位"),
            QDir(d->workspace).filePath(QStringLiteral("robot-teaching.json")), tr("JSON 文件 (*.json)"));
        if (!path.isEmpty()) {
            QString error;
            saveTeaching(path, &error);
        }
    });
    connect(d->load, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("加载机械臂示教点位"), d->workspace,
                                                          tr("JSON 文件 (*.json)"));
        if (!path.isEmpty()) {
            QString error;
            loadTeaching(path, &error);
        }
    });
    auto *timer = new QTimer(this);
    timer->setInterval(60);
    connect(timer, &QTimer::timeout, this, &RobotConsole::refresh);
    timer->start();
    refresh();
}

RobotConsole::~RobotConsole() {
    // Focus-out can commit editors while QWidget tears children down. Disconnect
    // those senders before the private UI state captured by their lambdas dies.
    for (auto *child : findChildren<QObject *>())
        QObject::disconnect(child, nullptr, this, nullptr);
}

void RobotConsole::setDevices(const QList<DeviceConfig> &devices) {
    const QString previous = d->id();
    QStringList ids;
    for (const auto &device : devices)
        if (device.category == QStringLiteral("robot") &&
            device.backend.compare(QStringLiteral("simulated"), Qt::CaseInsensitive) == 0)
            ids.append(device.id);
    QStringList shown;
    for (int i = 0; i < d->device->count(); ++i)
        shown.append(d->device->itemData(i).toString());
    if (shown != ids) {
        const QSignalBlocker blocker(d->device);
        d->device->clear();
        for (const auto &device : devices)
            if (ids.contains(device.id))
                d->device->addItem(QStringLiteral("%1 · %2").arg(device.id, device.model), device.id);
        const int index = d->device->findData(previous);
        if (index >= 0)
            d->device->setCurrentIndex(index);
        d->forceEdits = true;
    }
    // A model label can change while the device identity and its draft remain.
    for (const auto &device : devices) {
        const int index = d->device->findData(device.id);
        if (index >= 0)
            d->device->setItemText(index, QStringLiteral("%1 · %2").arg(device.id, device.model));
    }
    refresh();
}
void RobotConsole::setRunning(bool running) {
    d->running = running;
    refresh();
}
void RobotConsole::resetEdits() {
    d->forceEdits = true;
    d->boundId.clear();
    refresh();
}

void RobotConsole::refresh() {
    if (d->refreshing)
        return;
    d->refreshing = true;
    const auto snapshot = d->plant ? d->plant->snapshot() : SimulationSnapshot{};
    const auto *device = selectedRobot(snapshot, d->id());
    const bool available = device != nullptr;
    const SimRobotState state = available ? device->robot : SimRobotState{};
    const bool busy = state.programRunning;
    const bool unlocked = available && !d->running && !busy;
    d->manual = unlocked && device->connected && state.enabled && !snapshot.emergencyStop;
    const bool stopped = d->manual && !state.moving;
    const bool rebind =
        d->forceEdits || d->boundId != d->id() || d->generation != snapshot.interruptionGeneration;
    if (available && rebind) {
        const auto &target = state.target;
        const std::array<double, 4> joints{target.j1, target.j2, target.z, target.j4};
        const auto pose = robotForward(target);
        const std::array<double, 4> coordinates{pose.x, pose.y, pose.z, pose.r};
        for (int i = 0; i < 4; ++i) {
            d->joints[i].target->setValue(joints[i]);
            d->pose[i]->setValue(coordinates[i]);
        }
        d->speed->setValue(qRound(state.speedPercent));
        d->speedSlider->setValue(d->speed->value());
        d->elbow->setCurrentIndex(target.j2 >= 0 ? 0 : 1);
        d->boundId = d->id();
        d->forceEdits = false;
    }
    d->generation = snapshot.interruptionGeneration;
    d->add->setVisible(!available);
    d->add->setEnabled(!d->running);
    d->device->setEnabled(d->device->count() > 0);
    d->online->setEnabled(available);
    checked(d->online, available && device->connected);
    checked(d->enabled, available && state.enabled);
    d->enabled->setEnabled(unlocked && device->connected && !snapshot.emergencyStop);
    d->stop->setEnabled(available);
    d->emergency->setEnabled(d->plant != nullptr);
    d->emergency->setText(snapshot.emergencyStop ? tr("解除急停") : tr("虚拟急停"));
    QString status;
    if (!available)
        status = tr("暂无仿真机械臂");
    else if (snapshot.emergencyStop)
        status = tr("急停锁定 · 重新发令后才会运动");
    else if (!device->connected)
        status = tr("设备离线");
    else if (!state.enabled)
        status = tr("未使能");
    else if (!state.fault.isEmpty())
        status = state.fault;
    else if (busy)
        status = tr("%1 · %2 / %3").arg(state.programName).arg(state.programStep).arg(state.programSteps);
    else if (d->running)
        status = tr("检测流程接管中");
    else
        status = state.moving ? tr("运动中") : tr("就绪");
    d->state->setText(status);
    d->state->setStyleSheet(available && device->connected && !snapshot.emergencyStop && state.fault.isEmpty()
                                ? QStringLiteral("color:#258564;font-weight:600;")
                                : QStringLiteral("color:#bd4254;font-weight:600;"));
    if (available) {
        d->tcp->setText(tr("TCP   X %1   Y %2   Z %3 mm   R %4°")
                            .arg(state.tcp.x, 0, 'f', 1)
                            .arg(state.tcp.y, 0, 'f', 1)
                            .arg(state.tcp.z, 0, 'f', 1)
                            .arg(state.tcp.r, 0, 'f', 1));
        const QMap<QString, QString> locations{{QStringLiteral("feed"), tr("上料位")},
                                               {QStringLiteral("gripper"), tr("夹爪中")},
                                               {QStringLiteral("ok"), tr("OK 料箱")},
                                               {QStringLiteral("ng"), tr("NG 料箱")},
                                               {QStringLiteral("loose"), tr("散落在工作区")}};
        d->part->setText(
            tr("夹爪：%1  ·  工件：%2  ·  已分拣 OK %3 / NG %4")
                .arg(state.gripperClosed ? tr("闭合") : tr("张开"),
                     state.carryingPart ? tr("夹持中") : locations.value(state.partLocation, tr("无工件")))
                .arg(state.okCount)
                .arg(state.ngCount));
        d->scene->setState(state, device->connected, snapshot.emergencyStop);
        // Keep drafts in their editors while an executing segment owns the
        // marker and ghost. Idle previews follow the operator's chosen IK arm.
        if (state.moving || state.programRunning)
            d->scene->setTarget(robotForward(state.target), state.target.j2 >= 0);
        else
            d->scene->setTarget(d->targetPose(), d->elbow->currentData().toBool());
        const std::array<double, 4> actual{state.joints.j1, state.joints.j2, state.joints.z, state.joints.j4};
        for (int i = 0; i < 4; ++i)
            d->joints[i].actual->setText(QString::number(actual[i], 'f', 1));
    } else {
        d->tcp->setText(tr("选择或添加一台仿真 SCARA"));
        d->part->setText(tr("上料位 → 抓取 → OK / NG 料箱"));
        d->scene->clearState();
        for (auto &joint : d->joints)
            joint.actual->setText(QStringLiteral("—"));
    }
    d->scene->setEnabled(d->manual);
    for (auto &joint : d->joints) {
        joint.target->setEnabled(d->manual);
        joint.slider->setEnabled(d->manual);
        joint.minus->setEnabled(d->manual);
        joint.plus->setEnabled(d->manual);
    }
    for (auto *value : d->pose)
        value->setEnabled(d->manual);
    d->elbow->setEnabled(d->manual);
    d->speed->setEnabled(d->manual);
    d->speedSlider->setEnabled(d->manual);
    d->moveJoints->setEnabled(d->manual);
    d->moveTcp->setEnabled(d->manual);
    d->home->setEnabled(d->manual);
    d->open->setEnabled(stopped);
    d->close->setEnabled(stopped);
    d->restock->setEnabled(stopped && !state.carryingPart);
    d->sortOk->setEnabled(stopped);
    d->sortNg->setEnabled(stopped);
    d->teach->setEnabled(stopped && state.taughtPoints.size() < 100);
    d->name->setEnabled(stopped);
    d->load->setEnabled(stopped);
    d->save->setEnabled(available);
    d->table->setEnabled(available);
    d->points = available ? state.taughtPoints : QVector<RobotWaypoint>{};
    const auto json = robotTeachingToJson(d->points);
    if (json != d->shownTeaching || rebind) {
        const int selected = d->table->currentRow();
        d->shownTeaching = json;
        const QSignalBlocker blocker(d->table);
        d->table->setRowCount(d->points.size());
        for (int row = 0; row < d->points.size(); ++row) {
            const auto &point = d->points.at(row);
            const QStringList values{point.name,
                                     QString::number(point.joints.j1, 'f', 1),
                                     QString::number(point.joints.j2, 'f', 1),
                                     QString::number(point.joints.z, 'f', 1),
                                     QString::number(point.joints.j4, 'f', 1),
                                     point.gripperClosed ? tr("闭合") : tr("张开")};
            for (int col = 0; col < values.size(); ++col)
                d->table->setItem(row, col, new QTableWidgetItem(values.at(col)));
        }
        if (!d->points.isEmpty())
            d->table->selectRow(
                qBound(0, selected < 0 ? int(d->points.size()) - 1 : selected, int(d->points.size()) - 1));
    }
    const bool selected = d->table->currentRow() >= 0 && d->table->currentRow() < d->points.size();
    d->remove->setEnabled(stopped && selected);
    d->movePoint->setEnabled(stopped && selected);
    d->play->setEnabled(stopped && !d->points.isEmpty());
    d->teachingHint->setText(
        d->running ? tr("检测运行期间锁定手动操作与示教编辑，停止与急停保持可用。")
        : busy     ? tr("程序运行中；停止将取消剩余步骤。")
                   : tr("%1 / 100 点 · 加载文件会一次性校验并替换点位。").arg(d->points.size()));
    d->refreshing = false;
}

bool RobotConsole::saveTeaching(const QString &path, QString *error) const {
    QString problem;
    const auto snapshot = d->plant ? d->plant->snapshot() : SimulationSnapshot{};
    const auto *device = selectedRobot(snapshot, d->id());
    bool ok = false;
    if (!device)
        problem = tr("请先选择仿真机械臂。");
    else {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            problem = file.errorString();
        else {
            const QByteArray contents = QJsonDocument(robotTeachingToJson(device->robot.taughtPoints))
                                            .toJson(QJsonDocument::Indented);
            if (file.write(contents) != contents.size() || !file.commit())
                problem = file.errorString();
            else
                ok = true;
        }
    }
    if (error)
        *error = problem;
    d->report(ok, problem, tr("示教点位已保存。"));
    return ok;
}

bool RobotConsole::loadTeaching(const QString &path, QString *error) {
    QString problem;
    bool ok = false;
    const auto snapshot = d->plant ? d->plant->snapshot() : SimulationSnapshot{};
    const auto *device = selectedRobot(snapshot, d->id());
    if (!device)
        problem = tr("请先选择仿真机械臂。");
    else if (d->running || device->robot.programRunning || device->robot.moving)
        problem = tr("请先停止流程和机械臂，再加载示教点位。正常停止后不会自动回放。");
    else {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            problem = file.errorString();
        else if (file.size() > 2 * 1024 * 1024)
            problem = tr("示教文件过大。最多允许 100 个点位。");
        else {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
            QVector<RobotWaypoint> points;
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                problem = tr("JSON 文件格式错误：%1").arg(parseError.errorString());
            else if (robotTeachingFromJson(document.object(), &points, &problem))
                ok = d->plant->setRobotTeaching(d->id(), points, &problem);
        }
    }
    if (error)
        *error = problem;
    d->report(ok, problem, tr("示教点位已加载。"));
    return ok;
}

} // namespace ivs
