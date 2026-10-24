#include "ui/SimulationConsole.h"
#include "infrastructure/SimulationPlant.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <array>

namespace {
const ivs::SimDeviceState *findDevice(const ivs::SimulationSnapshot &snapshot, const QString &id) {
    for (const auto &device : snapshot.devices)
        if (device.id == id)
            return &device;
    return nullptr;
}
bool editing(QWidget *widget) {
    QWidget *focus = QApplication::focusWidget();
    return focus && (focus == widget || widget->isAncestorOf(focus));
}
template <typename Widget, typename Value>
void showValue(Widget *widget, const Value &value, bool force = false) {
    const QSignalBlocker blocker(widget);
    if (force || !editing(widget))
        widget->setValue(value);
}
void showChecked(QCheckBox *widget, bool checked) {
    const QSignalBlocker blocker(widget);
    widget->setChecked(checked);
}
} // namespace

namespace ivs {
struct SimulationConsole::Ui {
    struct Axis {
        QLabel *position, *status, *limits;
        QCheckBox *enabled;
        QDoubleSpinBox *target, *speed;
        QPushButton *minus, *plus, *move, *home, *stop;
        bool targetDraft = false, speedDraft = false;
    };
    struct Light {
        QCheckBox *enabled;
        QSlider *brightness;
        QSpinBox *value;
    };
    struct Signal {
        QCheckBox *discrete, *coil;
        QSpinBox *holding, *input;
    };
    std::shared_ptr<SimulationPlant> plant;
    QList<DeviceConfig> devices;
    bool running = false, refreshing = false;
    QString boundMotion, boundLight, boundSignal;
    int boundSignalPage = -1;
    bool forceEdits = false;
    QCheckBox *manual, *automatic, *connected;
    QPushButton *trigger, *emergency, *reset;
    QComboBox *partMode, *connectionDevice, *motionDevice, *lightDevice, *triggerDevice, *signalDevice,
        *signalPage;
    QLabel *pending, *notice, *summary, *manualHint;
    QTableWidget *signalTable;
    std::array<Axis, 4> axes;
    std::array<Light, 4> lights;
    std::array<Signal, 16> signalRows;
};

SimulationConsole::SimulationConsole(std::shared_ptr<SimulationPlant> plant, QWidget *parent)
    : QWidget(parent), d(std::make_unique<Ui>()) {
    d->plant = std::move(plant);
    setObjectName(QStringLiteral("simulationConsole"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 8, 24, 16);
    auto *headingRow = new QHBoxLayout;
    auto *heading = new QLabel(tr("仿真控制台"), this);
    heading->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:#173655;"));
    headingRow->addWidget(heading);
    headingRow->addStretch();
    d->summary = new QLabel(this);
    d->summary->setObjectName(QStringLiteral("simulationSummary"));
    headingRow->addWidget(d->summary);
    layout->addLayout(headingRow);
    auto *intro =
        new QLabel(tr("操作共享虚拟设备，观察运动、光照与信号对检测的影响。真实硬件不在此页控制。"), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    d->notice = new QLabel(tr("先选择虚拟设备；手动接管后可固定运动与亮度，关闭自动触发可逐件测试。"), this);
    d->notice->setObjectName(QStringLiteral("simulationMessage"));
    d->notice->setWordWrap(true);
    d->notice->setMinimumHeight(30);
    layout->addWidget(d->notice);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    layout->addWidget(scroll, 1);
    auto *body = new QWidget(scroll);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 4, 0);
    bodyLayout->setSpacing(12);
    scroll->setWidget(body);

    auto *controlCard = new QGroupBox(tr("运行条件与故障注入"), body);
    auto *controls = new QGridLayout(controlCard);
    controls->setHorizontalSpacing(12);
    d->manual = new QCheckBox(tr("手动接管运动与光源"), controlCard);
    d->manual->setObjectName(QStringLiteral("manualOverrideCheck"));
    controls->addWidget(d->manual, 0, 0);
    d->manualHint = new QLabel(controlCard);
    d->manualHint->setWordWrap(true);
    controls->addWidget(d->manualHint, 0, 1, 1, 3);
    d->emergency = new QPushButton(controlCard);
    d->emergency->setObjectName(QStringLiteral("simulationEmergencyButton"));
    d->emergency->setMinimumHeight(34);
    controls->addWidget(d->emergency, 0, 4);
    d->reset = new QPushButton(tr("重置虚拟状态"), controlCard);
    d->reset->setObjectName(QStringLiteral("simulationResetButton"));
    controls->addWidget(d->reset, 0, 5);
    d->automatic = new QCheckBox(tr("自动到位触发"), controlCard);
    d->automatic->setObjectName(QStringLiteral("automaticTriggerCheck"));
    controls->addWidget(d->automatic, 1, 0);
    d->triggerDevice = new QComboBox(controlCard);
    d->triggerDevice->setObjectName(QStringLiteral("triggerDeviceCombo"));
    d->triggerDevice->setToolTip(tr("单件触发只进入所选虚拟 PLC / I/O 的队列"));
    controls->addWidget(d->triggerDevice, 1, 1);
    d->trigger = new QPushButton(tr("触发一件"), controlCard);
    d->trigger->setObjectName(QStringLiteral("triggerOnceButton"));
    controls->addWidget(d->trigger, 1, 2);
    d->pending = new QLabel(controlCard);
    d->pending->setObjectName(QStringLiteral("pendingTriggerCount"));
    controls->addWidget(d->pending, 1, 3);
    controls->addWidget(new QLabel(tr("工件场景"), controlCard), 1, 4);
    d->partMode = new QComboBox(controlCard);
    d->partMode->setObjectName(QStringLiteral("partModeCombo"));
    d->partMode->addItem(tr("交替 OK / NG"), QStringLiteral("alternating"));
    d->partMode->addItem(tr("固定 OK"), QStringLiteral("good"));
    d->partMode->addItem(tr("固定 NG"), QStringLiteral("ng"));
    d->partMode->addItem(tr("无工件"), QStringLiteral("missing"));
    controls->addWidget(d->partMode, 1, 5);
    controls->addWidget(new QLabel(tr("连接故障"), controlCard), 2, 0);
    d->connectionDevice = new QComboBox(controlCard);
    d->connectionDevice->setObjectName(QStringLiteral("connectionDeviceCombo"));
    controls->addWidget(d->connectionDevice, 2, 1, 1, 2);
    d->connected = new QCheckBox(tr("虚拟设备在线"), controlCard);
    d->connected->setObjectName(QStringLiteral("simulationConnectedCheck"));
    controls->addWidget(d->connected, 2, 3);
    auto *faultHint = new QLabel(tr("解除急停或恢复连接后，请手动重新启动流程。"), controlCard);
    faultHint->setWordWrap(true);
    controls->addWidget(faultHint, 2, 4, 1, 2);
    bodyLayout->addWidget(controlCard);

    auto *motionCard = new QGroupBox(tr("虚拟运动平台 · 恒速位置模型"), body);
    auto *motionLayout = new QVBoxLayout(motionCard);
    auto *motionTop = new QHBoxLayout;
    d->motionDevice = new QComboBox(motionCard);
    d->motionDevice->setObjectName(QStringLiteral("motionDeviceCombo"));
    motionTop->addWidget(new QLabel(tr("运动设备"), motionCard));
    motionTop->addWidget(d->motionDevice, 1);
    motionTop->addWidget(new QLabel(tr("点动 ±1 mm · 关闭此页仍会推进运动"), motionCard));
    motionLayout->addLayout(motionTop);
    auto *axes = new QGridLayout;
    const QStringList headers{
        tr("轴"), tr("当前位置 mm"), tr("使能"), tr("目标 mm"), tr("速度 mm/s"), tr("点动"), {}, {}, {},
        {},       tr("软限位 mm"),   tr("状态")};
    for (int col = 0; col < headers.size(); ++col)
        axes->addWidget(new QLabel(headers.at(col), motionCard), 0, col);
    for (int i = 0; i < 4; ++i) {
        auto &axis = d->axes[i];
        const QString prefix = QStringLiteral("axis%1").arg(i);
        const int row = i + 1;
        axes->addWidget(new QLabel(QStringList{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"),
                                               QStringLiteral("R")}
                                       .at(i),
                                   motionCard),
                        row, 0);
        axis.position = new QLabel(QStringLiteral("0.000"), motionCard);
        axis.position->setObjectName(prefix + QStringLiteral("Position"));
        axis.position->setMinimumWidth(70);
        axes->addWidget(axis.position, row, 1);
        axis.enabled = new QCheckBox(motionCard);
        axis.enabled->setObjectName(prefix + QStringLiteral("Enabled"));
        axes->addWidget(axis.enabled, row, 2);
        axis.target = new QDoubleSpinBox(motionCard);
        axis.target->setObjectName(prefix + QStringLiteral("Target"));
        axis.target->setDecimals(3);
        axis.target->setRange(-1000000, 1000000);
        axis.target->setKeyboardTracking(false);
        axes->addWidget(axis.target, row, 3);
        axis.speed = new QDoubleSpinBox(motionCard);
        axis.speed->setObjectName(prefix + QStringLiteral("Speed"));
        axis.speed->setRange(0.001, 1000);
        axis.speed->setDecimals(3);
        axis.speed->setKeyboardTracking(false);
        axes->addWidget(axis.speed, row, 4);
        axis.minus = new QPushButton(QStringLiteral("−"), motionCard);
        axis.minus->setObjectName(prefix + QStringLiteral("JogMinus"));
        axis.plus = new QPushButton(QStringLiteral("+"), motionCard);
        axis.plus->setObjectName(prefix + QStringLiteral("JogPlus"));
        axis.move = new QPushButton(tr("移动"), motionCard);
        axis.move->setObjectName(prefix + QStringLiteral("Move"));
        axis.home = new QPushButton(tr("回零"), motionCard);
        axis.home->setObjectName(prefix + QStringLiteral("Home"));
        axis.stop = new QPushButton(tr("停止"), motionCard);
        axis.stop->setObjectName(prefix + QStringLiteral("Stop"));
        axes->addWidget(axis.minus, row, 5);
        axes->addWidget(axis.plus, row, 6);
        axes->addWidget(axis.move, row, 7);
        axes->addWidget(axis.home, row, 8);
        axes->addWidget(axis.stop, row, 9);
        axis.limits = new QLabel(motionCard);
        axis.limits->setObjectName(prefix + QStringLiteral("Limits"));
        axes->addWidget(axis.limits, row, 10);
        axis.status = new QLabel(motionCard);
        axis.status->setObjectName(prefix + QStringLiteral("Status"));
        axis.status->setMinimumWidth(70);
        axes->addWidget(axis.status, row, 11);
        connect(axis.target, &QDoubleSpinBox::valueChanged, this, [this, i] {
            if (!d->refreshing)
                d->axes[i].targetDraft = true;
        });
        connect(axis.speed, &QDoubleSpinBox::valueChanged, this, [this, i] {
            if (!d->refreshing)
                d->axes[i].speedDraft = true;
        });
        connect(axis.enabled, &QCheckBox::toggled, this, [this, i](bool enabled) {
            if (!d->plant || d->refreshing)
                return;
            QString error;
            const bool ok =
                d->plant->enableAxis(d->motionDevice->currentData().toString(), i, enabled, &error);
            report(ok, error);
        });
        connect(axis.move, &QPushButton::clicked, this, [this, i] {
            if (!d->plant)
                return;
            auto &axis = d->axes[i];
            axis.target->interpretText();
            axis.speed->interpretText();
            QString error;
            const bool ok = d->plant->moveAxis(d->motionDevice->currentData().toString(), i,
                                               axis.target->value(), axis.speed->value(), &error);
            if (ok) {
                axis.targetDraft = false;
                axis.speedDraft = false;
            }
            report(ok, error);
        });
        connect(axis.home, &QPushButton::clicked, this, [this, i] {
            if (!d->plant)
                return;
            QString error;
            const bool ok = d->plant->homeAxis(d->motionDevice->currentData().toString(), i,
                                               d->axes[i].speed->value(), &error);
            if (ok)
                d->axes[i].targetDraft = false;
            report(ok, error);
        });
        connect(axis.stop, &QPushButton::clicked, this, [this, i] {
            if (!d->plant)
                return;
            QString error;
            const bool ok = d->plant->stopAxis(d->motionDevice->currentData().toString(), i, &error);
            if (ok)
                d->axes[i].targetDraft = false;
            report(ok, error);
        });
        for (const auto &jog : {qMakePair(axis.minus, -1.0), qMakePair(axis.plus, 1.0)})
            connect(jog.first, &QPushButton::clicked, this, [this, i, delta = jog.second] {
                if (!d->plant)
                    return;
                const auto snapshot = d->plant->snapshot();
                const auto *device = findDevice(snapshot, d->motionDevice->currentData().toString());
                if (!device || i >= device->axes.size())
                    return;
                QString error;
                const bool ok = d->plant->moveAxis(device->id, i, device->axes.at(i).position + delta,
                                                   d->axes[i].speed->value(), &error);
                if (ok)
                    d->axes[i].targetDraft = false;
                report(ok, error);
            });
    }
    motionLayout->addLayout(axes);
    bodyLayout->addWidget(motionCard);

    auto *bottom = new QHBoxLayout;
    auto *lightCard = new QGroupBox(tr("虚拟光源"), body);
    auto *lightLayout = new QVBoxLayout(lightCard);
    d->lightDevice = new QComboBox(lightCard);
    d->lightDevice->setObjectName(QStringLiteral("lightDeviceCombo"));
    lightLayout->addWidget(d->lightDevice);
    auto *lightGrid = new QGridLayout;
    for (int i = 0; i < 4; ++i) {
        auto &light = d->lights[i];
        const QString prefix = QStringLiteral("light%1").arg(i);
        light.enabled = new QCheckBox(tr("CH%1").arg(i + 1), lightCard);
        light.enabled->setObjectName(prefix + QStringLiteral("Enabled"));
        lightGrid->addWidget(light.enabled, i, 0);
        light.brightness = new QSlider(Qt::Horizontal, lightCard);
        light.brightness->setObjectName(prefix + QStringLiteral("Brightness"));
        light.brightness->setRange(0, 255);
        light.brightness->setMinimumWidth(100);
        lightGrid->addWidget(light.brightness, i, 1);
        light.value = new QSpinBox(lightCard);
        light.value->setObjectName(prefix + QStringLiteral("Value"));
        light.value->setRange(0, 255);
        light.value->setKeyboardTracking(false);
        lightGrid->addWidget(light.value, i, 2);
        connect(light.enabled, &QCheckBox::toggled, this, [this, i] { writeLight(i); });
        connect(light.brightness, &QSlider::valueChanged, this, [this, i](int value) {
            if (d->refreshing)
                return;
            const QSignalBlocker blocker(d->lights[i].value);
            d->lights[i].value->setValue(value);
            writeLight(i);
        });
        connect(light.value, &QSpinBox::valueChanged, this, [this, i](int value) {
            if (d->refreshing)
                return;
            const QSignalBlocker blocker(d->lights[i].brightness);
            d->lights[i].brightness->setValue(value);
            writeLight(i);
        });
    }
    lightLayout->addLayout(lightGrid);
    auto *lightHint = new QLabel(tr("CH1 影响默认相机曝光；80 为示例正常光照。\n\n暗光 / "
                                    "关灯用于测试图像测量故障。\n接管后，流程节点保留此页设定。"),
                                 lightCard);
    lightHint->setWordWrap(true);
    lightLayout->addWidget(lightHint);
    lightLayout->addStretch();
    bottom->addWidget(lightCard, 2);
    auto *signalCard = new QGroupBox(tr("PLC / I/O 信号表"), body);
    auto *signalLayout = new QVBoxLayout(signalCard);
    auto *signalTop = new QHBoxLayout;
    d->signalDevice = new QComboBox(signalCard);
    d->signalDevice->setObjectName(QStringLiteral("signalDeviceCombo"));
    signalTop->addWidget(d->signalDevice, 1);
    d->signalPage = new QComboBox(signalCard);
    d->signalPage->setObjectName(QStringLiteral("signalPageCombo"));
    for (int i = 0; i < 4; ++i)
        d->signalPage->addItem(tr("地址 %1–%2").arg(i * 16).arg(i * 16 + 15), i * 16);
    signalTop->addWidget(d->signalPage);
    signalLayout->addLayout(signalTop);
    auto *signalHint =
        new QLabel(tr("DI0 可作为人工到位输入。仅虚拟设备允许强制 Discrete / Input。"), signalCard);
    signalHint->setWordWrap(true);
    signalLayout->addWidget(signalHint);
    d->signalTable = new QTableWidget(16, 5, signalCard);
    d->signalTable->setObjectName(QStringLiteral("simulationSignalTable"));
    d->signalTable->setHorizontalHeaderLabels(
        {tr("地址"), tr("DI 输入"), tr("DO 输出"), tr("保持寄存器"), tr("输入寄存器")});
    const QStringList tables{QStringLiteral("address"), QStringLiteral("discrete"), QStringLiteral("coil"),
                             QStringLiteral("holding"), QStringLiteral("input")};
    for (int column = 0; column < tables.size(); ++column)
        d->signalTable->horizontalHeaderItem(column)->setToolTip(tables.at(column));
    d->signalTable->verticalHeader()->hide();
    d->signalTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    d->signalTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    d->signalTable->setSelectionMode(QAbstractItemView::NoSelection);
    d->signalTable->setMinimumHeight(265);
    for (int row = 0; row < 16; ++row) {
        auto &signal = d->signalRows[row];
        auto *address = new QTableWidgetItem(QString::number(row));
        address->setFlags(Qt::ItemIsEnabled);
        d->signalTable->setItem(row, 0, address);
        signal.discrete = new QCheckBox(d->signalTable);
        signal.coil = new QCheckBox(d->signalTable);
        signal.holding = new QSpinBox(d->signalTable);
        signal.input = new QSpinBox(d->signalTable);
        for (auto *spin : {signal.holding, signal.input}) {
            spin->setRange(0, 65535);
            spin->setKeyboardTracking(false);
            spin->setFrame(false);
        }
        d->signalTable->setCellWidget(row, 1, signal.discrete);
        d->signalTable->setCellWidget(row, 2, signal.coil);
        d->signalTable->setCellWidget(row, 3, signal.holding);
        d->signalTable->setCellWidget(row, 4, signal.input);
        connect(signal.discrete, &QCheckBox::toggled, this,
                [this, row](bool value) { writeSignal(row, QStringLiteral("discrete"), value ? 1 : 0); });
        connect(signal.coil, &QCheckBox::toggled, this,
                [this, row](bool value) { writeSignal(row, QStringLiteral("coil"), value ? 1 : 0); });
        connect(signal.holding, &QSpinBox::valueChanged, this,
                [this, row](int value) { writeSignal(row, QStringLiteral("holding"), value); });
        connect(signal.input, &QSpinBox::valueChanged, this,
                [this, row](int value) { writeSignal(row, QStringLiteral("input"), value); });
    }
    signalLayout->addWidget(d->signalTable, 1);
    bottom->addWidget(signalCard, 3);
    bodyLayout->addLayout(bottom);
    bodyLayout->addStretch();
    connect(d->manual, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!d->plant || d->refreshing)
            return;
        d->plant->setManualOverride(enabled);
        report(true, {},
               enabled ? tr("已手动接管虚拟运动与光源。") : tr("虚拟运动与光源将由流程节点下发参数。"));
    });
    connect(d->automatic, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!d->plant || d->refreshing)
            return;
        d->plant->setAutomaticTrigger(enabled);
        refresh();
    });
    connect(d->trigger, &QPushButton::clicked, this, [this] {
        if (!d->plant)
            return;
        QString error;
        const bool ok = d->plant->queueTrigger(d->triggerDevice->currentData().toString(), &error);
        report(ok, error);
    });
    connect(d->emergency, &QPushButton::clicked, this, [this] {
        if (!d->plant)
            return;
        const bool emergency = !d->plant->snapshot().emergencyStop;
        d->plant->setEmergencyStop(emergency);
        report(true, {},
               emergency ? tr("仿真急停已生效，所有虚拟轴停止。")
                         : tr("仿真急停已解除；请手动重新启动流程。"));
    });
    connect(d->reset, &QPushButton::clicked, this, [this] {
        if (!d->plant || d->running || d->plant->snapshot().emergencyStop)
            return;
        QString error;
        const bool ok = d->plant->reset(&error);
        if (ok) {
            resetEdits();
        }
        report(ok, error, tr("虚拟状态已重置。"));
    });
    connect(d->partMode, &QComboBox::currentIndexChanged, this, [this] {
        if (!d->plant || d->refreshing)
            return;
        QString error;
        const bool ok = d->plant->setPartMode(d->partMode->currentData().toString(), &error);
        report(ok, error);
    });
    connect(d->connected, &QCheckBox::toggled, this, [this](bool connected) {
        if (!d->plant || d->refreshing)
            return;
        QString error;
        const bool ok =
            d->plant->setConnected(d->connectionDevice->currentData().toString(), connected, &error);
        report(ok, error,
               connected ? tr("虚拟连接已恢复；请手动重新启动流程。") : tr("已注入虚拟设备断线。"));
    });
    for (auto *combo : {d->connectionDevice, d->motionDevice, d->lightDevice, d->triggerDevice,
                        d->signalDevice, d->signalPage})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            if (!d->refreshing)
                refresh();
        });
    auto *timer = new QTimer(this);
    timer->setObjectName(QStringLiteral("simulationRefreshTimer"));
    timer->setInterval(80);
    connect(timer, &QTimer::timeout, this, &SimulationConsole::refresh);
    timer->start();
    refresh();
}

SimulationConsole::~SimulationConsole() {
    // Focus-out can commit a spin box while QWidget destroys its children.
    // Disconnect those callbacks before the private UI state is released.
    for (auto *child : findChildren<QObject *>())
        QObject::disconnect(child, nullptr, this, nullptr);
}

void SimulationConsole::setDevices(const QList<DeviceConfig> &devices) {
    d->devices.clear();
    for (const auto &device : devices)
        if (device.backend.compare(QStringLiteral("simulated"), Qt::CaseInsensitive) == 0)
            d->devices.append(device);
    rebuildSelectors();
    refresh();
}

void SimulationConsole::rebuildSelectors() {
    d->refreshing = true;
    auto fill = [this](QComboBox *combo, const QStringList &categories) {
        const QString selected = combo->currentData().toString();
        QStringList ids, existing;
        for (const auto &device : d->devices)
            if (categories.isEmpty() || categories.contains(device.category))
                ids.append(device.id);
        for (int i = 0; i < combo->count(); ++i)
            existing.append(combo->itemData(i).toString());
        if (ids == existing)
            return;
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto &device : d->devices)
            if (categories.isEmpty() || categories.contains(device.category))
                combo->addItem(QStringLiteral("%1 · %2").arg(device.id, device.model), device.id);
        const int previous = combo->findData(selected);
        if (previous >= 0)
            combo->setCurrentIndex(previous);
    };
    fill(d->connectionDevice, {});
    fill(d->motionDevice, {QStringLiteral("motion")});
    fill(d->lightDevice, {QStringLiteral("light")});
    fill(d->triggerDevice, {QStringLiteral("plc"), QStringLiteral("io")});
    fill(d->signalDevice, {QStringLiteral("plc"), QStringLiteral("io")});
    d->refreshing = false;
}

void SimulationConsole::setRunning(bool running) {
    d->running = running;
    refresh();
}

void SimulationConsole::resetEdits() {
    d->boundMotion.clear();
    d->boundLight.clear();
    d->boundSignal.clear();
    d->boundSignalPage = -1;
    d->forceEdits = true;
    for (auto &axis : d->axes) {
        axis.targetDraft = false;
        axis.speedDraft = false;
    }
    refresh();
}

void SimulationConsole::report(bool success, const QString &error, const QString &successText) {
    if (!success || !successText.isEmpty()) {
        const QString text = success ? successText : (error.isEmpty() ? tr("虚拟设备操作失败。") : error);
        d->notice->setText(text);
        d->notice->setStyleSheet(success ? QStringLiteral("color:#246046;")
                                         : QStringLiteral("color:#b42318;font-weight:600;"));
        emit message(success ? QStringLiteral("INFO") : QStringLiteral("ERROR"), text);
    }
    refresh();
}

void SimulationConsole::writeLight(int channel) {
    if (!d->plant || d->refreshing)
        return;
    const auto &light = d->lights[channel];
    QString error;
    const bool ok = d->plant->setLight(d->lightDevice->currentData().toString(), channel,
                                       light.enabled->isChecked(), light.brightness->value(), &error);
    report(ok, error);
}

void SimulationConsole::writeSignal(int row, const QString &table, int value) {
    if (!d->plant || d->refreshing)
        return;
    QString error;
    const int address = d->signalPage->currentData().toInt() + row;
    const bool ok = d->plant->writeSignal(d->signalDevice->currentData().toString(), table, address,
                                          quint16(value), &error);
    report(ok, error);
}

void SimulationConsole::refresh() {
    if (d->refreshing)
        return;
    d->refreshing = true;
    const auto snapshot = d->plant ? d->plant->snapshot() : SimulationSnapshot{};
    const bool available = bool(d->plant);
    const auto *motion = findDevice(snapshot, d->motionDevice->currentData().toString());
    const auto *light = findDevice(snapshot, d->lightDevice->currentData().toString());
    const auto *signalState = findDevice(snapshot, d->signalDevice->currentData().toString());
    const auto *trigger = findDevice(snapshot, d->triggerDevice->currentData().toString());
    const auto *connection = findDevice(snapshot, d->connectionDevice->currentData().toString());
    int connectedCount = 0;
    for (const auto &device : snapshot.devices)
        if (device.connected)
            ++connectedCount;
    d->summary->setText(
        tr("%1 / %2 设备在线 · %3")
            .arg(connectedCount)
            .arg(snapshot.devices.size())
            .arg(snapshot.emergencyStop ? tr("急停中") : (d->running ? tr("流程运行中") : tr("待机"))));
    showChecked(d->manual, snapshot.manualOverride);
    showChecked(d->automatic, snapshot.automaticTrigger);
    d->manual->setEnabled(available);
    d->automatic->setEnabled(available);
    d->manualHint->setText(snapshot.manualOverride ? tr("已接管：流程保留此页的运动与亮度值。")
                                                   : tr("未接管：流程节点会按参数更新运动与光源。"));
    d->emergency->setEnabled(available);
    d->emergency->setText(snapshot.emergencyStop ? tr("解除仿真急停") : tr("仿真急停"));
    d->emergency->setStyleSheet(
        snapshot.emergencyStop
            ? QStringLiteral("background:#b42318;color:white;font-weight:600;")
            : QStringLiteral("background:#fff1f0;color:#b42318;font-weight:600;border:1px solid #edb2ac;"));
    d->reset->setEnabled(available && !d->running && !snapshot.emergencyStop);
    d->reset->setToolTip(d->running ? tr("停止流程后才可重置虚拟状态")
                                    : tr("恢复所有虚拟轴、光源、信号与触发队列的默认值"));
    d->trigger->setEnabled(trigger && trigger->connected && !snapshot.emergencyStop);
    d->pending->setText(tr("待触发：%1 件").arg(trigger ? trigger->pendingTriggers : 0));
    d->connected->setEnabled(connection);
    showChecked(d->connected, connection && connection->connected);
    {
        const QSignalBlocker blocker(d->partMode);
        d->partMode->setCurrentIndex(d->partMode->findData(snapshot.partMode));
        d->partMode->setEnabled(available);
    }
    const bool newMotion = d->forceEdits || d->boundMotion != (motion ? motion->id : QString());
    d->boundMotion = motion ? motion->id : QString();
    for (int i = 0; i < 4; ++i) {
        auto &controls = d->axes[i];
        const bool exists = motion && i < motion->axes.size();
        const SimAxisState state = exists ? motion->axes.at(i) : SimAxisState{};
        const bool operational = exists && motion->connected && !snapshot.emergencyStop;
        controls.position->setText(exists ? QString::number(state.position, 'f', 3) : QStringLiteral("—"));
        showChecked(controls.enabled, exists && state.enabled);
        controls.enabled->setEnabled(operational);
        controls.target->setEnabled(operational);
        controls.speed->setEnabled(operational);
        if (newMotion) {
            controls.targetDraft = false;
            controls.speedDraft = false;
        }
        if (!controls.targetDraft)
            showValue(controls.target, state.target, newMotion);
        if (!controls.speedDraft)
            showValue(controls.speed, state.speed, newMotion);
        for (auto *button : {controls.minus, controls.plus, controls.move, controls.home})
            button->setEnabled(operational && state.enabled);
        controls.stop->setEnabled(exists);
        controls.limits->setText(exists ? QStringLiteral("%1 … %2").arg(state.minimum).arg(state.maximum)
                                        : QStringLiteral("—"));
        QString status = tr("无设备");
        if (exists) {
            if (!motion->connected)
                status = tr("断线");
            else if (!state.fault.isEmpty())
                status = state.fault;
            else if (snapshot.emergencyStop)
                status = tr("急停");
            else if (!state.enabled)
                status = tr("未使能");
            else if (state.moving)
                status = tr("运动中");
            else
                status = state.homed ? tr("已回零 / 就绪") : tr("就绪");
        }
        controls.status->setText(status);
        controls.status->setToolTip(status);
    }
    const bool newLight = d->forceEdits || d->boundLight != (light ? light->id : QString());
    d->boundLight = light ? light->id : QString();
    for (int i = 0; i < 4; ++i) {
        auto &controls = d->lights[i];
        const bool exists = light && i < light->lights.size();
        const auto state = exists ? light->lights.at(i) : SimLightState{};
        const bool operational = exists && light->connected && !snapshot.emergencyStop;
        controls.enabled->setEnabled(operational);
        controls.brightness->setEnabled(operational);
        controls.value->setEnabled(operational);
        showChecked(controls.enabled, exists && state.enabled);
        showValue(controls.brightness, state.brightness, newLight);
        showValue(controls.value, state.brightness, newLight);
    }
    const int firstAddress = d->signalPage->currentData().toInt();
    const bool newSignal = d->forceEdits || d->boundSignal != (signalState ? signalState->id : QString()) ||
                           d->boundSignalPage != firstAddress;
    d->boundSignal = signalState ? signalState->id : QString();
    d->boundSignalPage = firstAddress;
    for (int row = 0; row < 16; ++row) {
        auto &controls = d->signalRows[row];
        const int address = firstAddress + row;
        const QString prefix = QStringLiteral("signal%1").arg(address);
        controls.discrete->setObjectName(prefix + QStringLiteral("Discrete"));
        controls.coil->setObjectName(prefix + QStringLiteral("Coil"));
        controls.holding->setObjectName(prefix + QStringLiteral("Holding"));
        controls.input->setObjectName(prefix + QStringLiteral("Input"));
        d->signalTable->item(row, 0)->setText(QString::number(address));
        const bool operational = signalState && signalState->connected;
        controls.discrete->setEnabled(operational && address < signalState->discreteInputs.size());
        controls.coil->setEnabled(operational && address < signalState->coils.size());
        controls.holding->setEnabled(operational && address < signalState->holdingRegisters.size());
        controls.input->setEnabled(operational && address < signalState->inputRegisters.size());
        showChecked(controls.discrete, signalState && address < signalState->discreteInputs.size() &&
                                           signalState->discreteInputs.at(address));
        showChecked(controls.coil,
                    signalState && address < signalState->coils.size() && signalState->coils.at(address));
        showValue(controls.holding,
                  signalState && address < signalState->holdingRegisters.size()
                      ? signalState->holdingRegisters.at(address)
                      : 0,
                  newSignal);
        showValue(controls.input,
                  signalState && address < signalState->inputRegisters.size()
                      ? signalState->inputRegisters.at(address)
                      : 0,
                  newSignal);
    }
    d->forceEdits = false;
    d->refreshing = false;
}
} // namespace ivs
