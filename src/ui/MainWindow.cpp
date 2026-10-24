#include "ui/MainWindow.h"
#include "application/PipelineWorker.h"
#include "application/Publisher.h"
#include "domain/ImageAnalysis.h"
#include "domain/Project.h"
#include "infrastructure/Devices.h"
#include "infrastructure/RecordRepository.h"
#include "infrastructure/SimulationPlant.h"
#include "ui/ChartWidget.h"
#include "ui/FlowEditor.h"
#include "ui/ImageView.h"
#include "ui/PanelEditor.h"
#include "ui/RecordTableModel.h"
#include "ui/RobotConsole.h"
#include "ui/SimulationConsole.h"

#include <QDesktopServices>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSysInfo>
#include <QtConcurrent>
#include <QtWidgets>

namespace ivs {
namespace {
QPushButton *button(const QString &text, const QString &name = {}) {
    auto *b = new QPushButton(text);
    if (!name.isEmpty())
        b->setObjectName(name);
    b->setCursor(Qt::PointingHandCursor);
    return b;
}
QLabel *heading(const QString &text, int size = 20) {
    auto *label = new QLabel(text);
    label->setStyleSheet(QString("font-size:%1px;font-weight:600;").arg(size));
    return label;
}
QLabel *hint(const QString &text) {
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setObjectName("hint");
    return label;
}
QTableView *recordView(RecordTableModel *model) {
    auto *view = new QTableView;
    view->setModel(model);
    view->setAlternatingRowColors(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->verticalHeader()->hide();
    view->horizontalHeader()->setStretchLastSection(true);
    view->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    view->setMinimumHeight(205);
    return view;
}
QDoubleSpinBox *decimal(double value, double min = 0, double max = 100000) {
    auto *box = new QDoubleSpinBox;
    box->setDecimals(3);
    box->setRange(min, max);
    box->setValue(value);
    box->setSingleStep(.1);
    return box;
}
QComboBox *choices(const QStringList &items) {
    auto *c = new QComboBox;
    c->addItems(items);
    return c;
}
QString typeLabel(const QString &type) {
    return QHash<QString, QString>{{"camera", "相机"}, {"motion", "运动控制"}, {"light", "光源"},
                                   {"plc", "PLC"},     {"io", "数字 I/O"},     {"robot", "机械臂"}}
        .value(type, type);
}
struct HistoryPage {
    QList<DetectionRecord> records;
    qint64 count = 0;
    QString error;
    qint64 elapsed = 0;
};
struct JobResult {
    bool success = false;
    QString error;
    qint64 count = 0;
};
} // namespace

MainWindow::MainWindow(const QString &workspace, const QString &projectPath, bool runtimeMode,
                       QWidget *parent)
    : QMainWindow(parent), m_workspace(QFileInfo(workspace).absoluteFilePath()), m_projectPath(projectPath),
      m_project(defaultProject()), m_runtime(runtimeMode) {
    QDir().mkpath(m_workspace);
    if (!m_projectPath.isEmpty()) {
        QString error;
        if (!loadProject(m_projectPath, &m_project, &error)) {
            m_logLines << "[ERROR] 工程加载失败: " + error;
            if (m_runtime)
                throw std::runtime_error(error.toStdString());
            m_projectPath.clear();
        }
    }
    qRegisterMetaType<ProjectConfig>();
    qRegisterMetaType<InspectionResult>();
    qRegisterMetaType<DetectionRecord>();
    m_simulation = std::make_shared<SimulationPlant>();
    buildUi();
    populateProject();
    m_worker = new PipelineWorker;
    m_worker->setSimulationPlant(m_simulation);
    m_worker->moveToThread(&m_pipelineThread);
    connect(&m_pipelineThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &PipelineWorker::stateChanged, this, &MainWindow::setRunState);
    connect(m_worker, &PipelineWorker::resultReady, this, &MainWindow::receiveResult);
    connect(m_worker, &PipelineWorker::logMessage, this, &MainWindow::appendLog);
    connect(m_worker, &PipelineWorker::nodeActivated, this, [this](const QString &, const QString &title) {
        statusBar()->showMessage("执行节点 · " + title);
    });
    m_pipelineThread.setObjectName("InspectionWorker");
    m_pipelineThread.start();
    appendLog("INFO", "工作台就绪 · " + algorithmBackend());
    appendLog("INFO", "工作目录: " + m_workspace);
    if (m_runtime)
        appendLog("INFO", "独立客户运行模式：设计与设备编辑已锁定");
    setRunState("idle");
    QTimer::singleShot(200, this, [this] { refreshHistory(); });
}
MainWindow::~MainWindow() {
    if (m_seedCancel)
        m_seedCancel->store(true);
    if (m_pipelineThread.isRunning()) {
        QMetaObject::invokeMethod(m_worker, &PipelineWorker::shutdown, Qt::BlockingQueuedConnection);
        m_pipelineThread.quit();
        m_pipelineThread.wait();
    }
    QThreadPool::globalInstance()->waitForDone();
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_publishBusy) {
        QMessageBox::information(this, "正在发布", "请等待运行包发布结束后关闭。");
        event->ignore();
        return;
    }
    if (!confirmDiscardOrSave()) {
        event->ignore();
        return;
    }
    if (m_seedCancel)
        m_seedCancel->store(true);
    event->accept();
}
void MainWindow::buildUi() {
    setWindowTitle("Industrial Vision Studio · 工业视觉工作台[*]");
    resize(1500, 1020);
    setMinimumSize(1120, 760);
    setStyleSheet(R"(
        QMainWindow,QWidget#workspace { background:#f3f5f9; color:#23324a; }
        QWidget { font-family:"PingFang SC","Microsoft YaHei","Noto Sans CJK SC",sans-serif; font-size:13px; }
        QWidget#sidebar {background:#14243b;} QWidget#sidebar QLabel {color:#b8c8dd;background:transparent;}
        QListWidget#navigation {background:transparent;border:0;color:#aebfd5;outline:0;}
        QListWidget#navigation::item {padding:15px 14px;margin:3px 8px;border-radius:7px;}
        QListWidget#navigation::item:selected {background:#274466;color:white;border-left:3px solid #42d7b0;}
        QFrame#topbar {background:white;border-bottom:1px solid #e1e6ef;}
        QFrame#card,QGroupBox {background:white;border:1px solid #e1e6ef;border-radius:9px;}
        QGroupBox {font-weight:600;margin-top:12px;padding:16px 10px 10px;}
        QGroupBox::title {subcontrol-origin:margin;left:16px;padding:0 5px;color:#31445f;}
        QLabel#hint {color:#77849a;font-size:12px;} QLabel#status {background:#e4f6ef;color:#14795d;padding:8px 16px;border-radius:16px;font-weight:bold;}
        QPushButton {background:white;border:1px solid #dce3ec;border-radius:6px;padding:8px 14px;color:#334763;min-height:20px;}
        QPushButton:hover {background:#edf3ff;border-color:#9db8ed;} QPushButton:disabled {color:#aab2c0;background:#f4f5f7;border-color:#e6e9ef;}
        QPushButton#primary {background:#2267e8;color:white;border-color:#2267e8;font-weight:600;}
        QPushButton#primary:hover {background:#1857ca;} QPushButton#stop {color:#c14b5e;border-color:#edccd2;}
        QLineEdit,QSpinBox,QDoubleSpinBox,QDateTimeEdit,QComboBox,QPlainTextEdit {background:white;border:1px solid #dce3ec;border-radius:5px;padding:6px;selection-background-color:#296ee9;}
        QTableView,QTableWidget {background:white;alternate-background-color:#f7f9fc;border:1px solid #e6eaf1;gridline-color:#edf0f5;selection-background-color:#e9f1ff;selection-color:#234169;}
        QHeaderView::section {background:#f5f7fb;color:#6b7a91;border:0;border-bottom:1px solid #e0e6ef;padding:9px;font-weight:600;}
        QScrollArea {border:0;background:transparent;} QStatusBar {background:white;color:#6f7e95;}
        QSplitter::handle {background:#e5eaf2;}
    )");
    auto *central = new QWidget;
    central->setObjectName("workspace");
    auto *root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(205);
    auto *side = new QVBoxLayout(sidebar);
    side->setContentsMargins(10, 26, 10, 20);
    auto *brand = heading("VISION / STUDIO", 18);
    brand->setStyleSheet("color:white;font-size:18px;font-weight:600;");
    side->addWidget(brand);
    side->addWidget(hint("工业视觉 · 通用软件框架"));
    side->addSpacing(28);
    m_navigation = new QListWidget;
    m_navigation->setObjectName("navigation");
    m_navigation->addItems({"◉   实时工作台", "▤   流程编排", "▣   设备配置", "▦   页面设计", "◷   历史数据",
                            "⚙   工程与发布", "◇   仿真控制台", "♧   机械臂实验室"});
    side->addWidget(m_navigation, 1);
    side->addWidget(hint("C++  /  Qt 6  /  CMake"));
    side->addWidget(hint("模拟设备可完整运行\n真实硬件按适配状态接入"));
    root->addWidget(sidebar);
    auto *main = new QVBoxLayout;
    main->setSpacing(0);
    root->addLayout(main, 1);
    auto *top = new QFrame;
    top->setObjectName("topbar");
    auto *topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(24, 14, 24, 14);
    m_projectLabel = heading(m_project.name, 17);
    topLayout->addWidget(m_projectLabel);
    topLayout->addStretch();
    m_status = new QLabel("就绪");
    m_status->setObjectName("status");
    topLayout->addWidget(m_status);
    main->addWidget(top);
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(24, 14, 24, 12);
    actions->setSpacing(8);
    m_new = button("新建");
    m_new->setObjectName("newProjectButton");
    m_open = button("打开工程");
    m_save = button("保存工程");
    m_start = button("▶  连续运行", "primary");
    m_start->setObjectName("startRunButton");
    m_start->setStyleSheet("background:#2267e8;color:white;border-color:#2267e8;font-weight:bold;");
    m_single = button("单次检测", "singleRunButton");
    m_stop = button("■  停止", "stop");
    m_publish = button("发布运行包 ↗");
    for (auto *b : {m_new, m_open, m_save})
        actions->addWidget(b);
    actions->addStretch();
    for (auto *b : {m_start, m_single, m_stop, m_publish})
        actions->addWidget(b);
    main->addLayout(actions);
    m_pages = new QStackedWidget;
    main->addWidget(m_pages, 1);
    m_liveModel = new RecordTableModel(this);
    m_historyModel = new RecordTableModel(this);
    m_pages->addWidget(buildDashboard());
    auto *flowPage = new QWidget;
    auto *flowLayout = new QVBoxLayout(flowPage);
    flowLayout->setContentsMargins(24, 8, 24, 20);
    flowLayout->addWidget(heading("可视化流程编排"));
    flowLayout->addWidget(
        hint("从左侧拖入节点，按列表顺序执行。采集 → 测量 → 判定 → 保存为必要链路；运行前自动校验。"));
    m_flowEditor = new FlowEditor;
    flowLayout->addWidget(m_flowEditor, 1);
    m_pages->addWidget(flowPage);
    m_pages->addWidget(buildDevices());
    auto *panelPage = new QWidget;
    auto *panelLayout = new QVBoxLayout(panelPage);
    panelLayout->setContentsMargins(24, 8, 24, 20);
    panelLayout->addWidget(heading("页面设计与数据绑定"));
    panelLayout->addWidget(
        hint("添加面板、调整顺序并选择结果字段。保存后自动应用至实时工作台，工程重开和发布时保留。"));
    m_panelEditor = new PanelEditor;
    panelLayout->addWidget(m_panelEditor, 1);
    m_pages->addWidget(panelPage);
    m_pages->addWidget(buildHistory());
    m_pages->addWidget(buildSettings());
    m_simulationConsole = new SimulationConsole(m_simulation);
    m_pages->addWidget(m_simulationConsole);
    connect(m_simulationConsole, &SimulationConsole::message, this, &MainWindow::appendLog);
    m_robotConsole = new RobotConsole(m_simulation, m_workspace);
    m_pages->addWidget(m_robotConsole);
    connect(m_robotConsole, &RobotConsole::message, this, &MainWindow::appendLog);
    connect(m_robotConsole, &RobotConsole::addRobotRequested, this, [this] {
        if (m_running || m_runtime)
            return;
        addDevice("robot", "simulated");
        ProjectConfig project;
        QString error;
        if (collectProject(&project, &error) && m_simulation->configure(project.devices, &error)) {
            m_simulationConsole->setDevices(project.devices);
            m_robotConsole->setDevices(project.devices);
            appendLog("INFO", "已添加 SCARA 仿真机械臂，可保存工程复用。");
        } else {
            reportError(error);
        }
    });
    connect(m_navigation, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    connect(m_navigation, &QListWidget::currentRowChanged, this, [this](int row) {
        if ((row != 6 && row != 7) || m_running || m_loading)
            return;
        ProjectConfig project;
        QString error;
        if (collectProject(&project, &error) && m_simulation->configure(project.devices, &error)) {
            m_simulation->setPixelsPerMm(project.pixelsPerMm);
            m_simulationConsole->setDevices(project.devices);
            m_robotConsole->setDevices(project.devices);
        } else {
            appendLog("ERROR", "仿真设备未更新，请修正工程参数: " + error);
        }
    });
    m_navigation->setCurrentRow(0);
    connect(m_new, &QPushButton::clicked, this, &MainWindow::newProject);
    connect(m_open, &QPushButton::clicked, this, &MainWindow::openProject);
    connect(m_save, &QPushButton::clicked, this, &MainWindow::saveCurrentProject);
    connect(m_start, &QPushButton::clicked, this, [this] { startRun(false); });
    connect(m_single, &QPushButton::clicked, this, [this] { startRun(true); });
    connect(m_stop, &QPushButton::clicked, this, &MainWindow::stopRun);
    connect(m_publish, &QPushButton::clicked, this, &MainWindow::publish);
    connect(m_panelEditor, &PanelEditor::panelsChanged, this, [this] {
        if (!m_loading) {
            m_project.panels = m_panelEditor->panels();
            rebuildDashboard();
        }
    });
    if (m_runtime) {
        for (int i : {1, 2, 3, 5})
            m_navigation->item(i)->setHidden(true);
        for (auto *b : {m_new, m_open, m_save, m_publish})
            b->hide();
    }
    setCentralWidget(central);
    statusBar()->showMessage("就绪 · 尚未启动检测");
    const auto markDirty = [this] {
        if (!m_loading && !m_runtime) {
            m_dirty = true;
            setWindowModified(true);
        }
    };
    connect(m_flowEditor, &FlowEditor::nodesChanged, this, markDirty);
    connect(m_panelEditor, &PanelEditor::panelsChanged, this, markDirty);
    connect(m_deviceTable, &QTableWidget::cellChanged, this, [markDirty](int, int) { markDirty(); });
    connect(m_deviceTable->model(), &QAbstractItemModel::rowsInserted, this, [markDirty] { markDirty(); });
    connect(m_deviceTable->model(), &QAbstractItemModel::rowsRemoved, this, [markDirty] { markDirty(); });
    for (auto *input : m_pages->widget(5)->findChildren<QLineEdit *>())
        connect(input, &QLineEdit::textChanged, this, markDirty);
    for (auto *input : m_pages->widget(5)->findChildren<QComboBox *>())
        connect(input, &QComboBox::currentIndexChanged, this, markDirty);
    for (auto *input : m_pages->widget(5)->findChildren<QCheckBox *>())
        connect(input, &QCheckBox::toggled, this, markDirty);
}
QWidget *MainWindow::buildDashboard() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 4, 24, 16);
    layout->setSpacing(14);
    auto *intro = new QHBoxLayout;
    intro->addWidget(heading("生产运行概览", 20));
    intro->addStretch();
    auto *liveHint = hint("LIVE  /  本次运行统计");
    liveHint->setWordWrap(false);
    intro->addWidget(liveHint);
    layout->addLayout(intro);
    auto *cards = new QHBoxLayout;
    cards->setSpacing(12);
    const QList<QPair<QString, QString>> values{
        {"本次已保存", "0"}, {"合格率", "—"}, {"当前直径", "— mm"}, {"检测耗时", "— ms"}};
    QList<QLabel *> labels;
    for (const auto &value : values) {
        auto *card = new QFrame;
        card->setObjectName("card");
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(18, 14, 18, 14);
        cardLayout->addWidget(hint(value.first));
        auto *v = heading(value.second, 26);
        v->setStyleSheet("color:#1d3f73;font-size:26px;font-weight:600;");
        cardLayout->addWidget(v);
        labels.append(v);
        cards->addWidget(card, 1);
    }
    m_totalValue = labels[0];
    m_totalValue->setObjectName("sessionRecordCount");
    m_yieldValue = labels[1];
    m_measureValue = labels[2];
    m_timeValue = labels[3];
    layout->addLayout(cards);
    m_dashboardScroll = new QScrollArea;
    m_dashboardScroll->setWidgetResizable(true);
    layout->addWidget(m_dashboardScroll, 1);
    return page;
}
void MainWindow::rebuildDashboard() {
    m_imageViews.clear();
    m_charts.clear();
    m_logViews.clear();
    auto *content = new QWidget;
    auto *grid = new QGridLayout(content);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(14);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    int index = 0;
    for (const auto &panel : m_project.panels) {
        auto *box = new QGroupBox(panel.title);
        auto *layout = new QVBoxLayout(box);
        layout->setContentsMargins(10, 16, 10, 10);
        if (panel.type == "image") {
            auto *view = new ImageView;
            view->setMinimumHeight(300);
            if (!m_lastResult.frame.isNull())
                view->setResult(m_lastResult);
            m_imageViews.append(view);
            layout->addWidget(view);
        } else if (panel.type == "table") {
            auto *view = recordView(m_liveModel);
            view->setMinimumHeight(300);
            layout->addWidget(view);
        } else if (panel.type == "trend" || panel.type == "scatter") {
            auto *chart = new ChartWidget(panel.type == "trend" ? ChartWidget::Kind::Trend
                                                                : ChartWidget::Kind::Scatter);
            chart->setField(panel.field);
            chart->setRecords(m_recentRecords);
            chart->setMinimumHeight(230);
            m_charts.append(chart);
            layout->addWidget(chart);
        } else if (panel.type == "log") {
            auto *log = new QPlainTextEdit;
            log->setReadOnly(true);
            log->setMaximumBlockCount(500);
            log->setPlainText(m_logLines.join('\n'));
            log->setMinimumHeight(160);
            m_logViews.append(log);
            layout->addWidget(log);
        }
        grid->addWidget(box, index / 2, index % 2);
        ++index;
    }
    if (index % 2)
        grid->addWidget(hint("在“页面设计”中添加或调整面板。"), index / 2, 1);
    grid->setRowStretch((index + 1) / 2, 1);
    auto *previous = m_dashboardScroll->takeWidget();
    m_dashboardScroll->setWidget(content);
    delete previous;
}
QWidget *MainWindow::buildDevices() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 8, 24, 20);
    layout->addWidget(heading("设备连接与能力目录"));
    layout->addWidget(
        hint("默认使用模拟设备。Modbus TCP "
             "为已实现协议；厂商专用相机、运动卡和光源型号仅列为计划适配，缺少适配器时会明确拒绝启动。"));
    auto *bar = new QHBoxLayout;
    auto *sim = button("添加模拟相机");
    auto *plc = button("添加 Modbus PLC");
    auto *io = button("添加 Modbus I/O");
    auto *robot = button("添加仿真机械臂", "addRobotDeviceButton");
    auto *remove = button("删除选中设备");
    for (auto *b : {sim, plc, io, robot, remove})
        bar->addWidget(b);
    bar->addStretch();
    layout->addLayout(bar);
    m_deviceTable = new QTableWidget;
    m_deviceTable->setColumnCount(8);
    m_deviceTable->setHorizontalHeaderLabels(
        {"设备 ID", "类型", "型号 / 名称", "接入后端", "主机", "端口", "Unit ID", "参数 JSON"});
    m_deviceTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_deviceTable->horizontalHeader()->setStretchLastSection(true);
    m_deviceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    layout->addWidget(m_deviceTable, 2);
    auto *catalog = new QTableWidget;
    catalog->setColumnCount(4);
    catalog->setHorizontalHeaderLabels({"设备类别", "候选型号", "接口", "支持状态"});
    catalog->setEditTriggers(QAbstractItemView::NoEditTriggers);
    catalog->setAlternatingRowColors(true);
    catalog->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    catalog->horizontalHeader()->setStretchLastSection(true);
    const auto descriptors = hardwareCatalog();
    catalog->setRowCount(descriptors.size());
    for (int row = 0; row < descriptors.size(); ++row) {
        const auto &d = descriptors[row];
        const QString status = d.status.contains("unavailable") ? "计划适配 / 当前不可连接"
                               : d.backend == "simulated"       ? "可运行 · 模拟"
                                                                : "协议已实现 / 型号映射待实机验证";
        const QStringList cells{typeLabel(d.category), d.model, d.interfaceName, status};
        for (int col = 0; col < cells.size(); ++col)
            catalog->setItem(row, col, new QTableWidgetItem(cells[col]));
    }
    layout->addWidget(heading("硬件支持矩阵", 15));
    layout->addWidget(catalog, 2);
    connect(sim, &QPushButton::clicked, this, [this] { addDevice("camera", "simulated"); });
    connect(plc, &QPushButton::clicked, this, [this] { addDevice("plc", "modbus-tcp"); });
    connect(io, &QPushButton::clicked, this, [this] { addDevice("io", "modbus-tcp"); });
    connect(robot, &QPushButton::clicked, this, [this] { addDevice("robot", "simulated"); });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (!m_running && m_deviceTable->currentRow() >= 0)
            m_deviceTable->removeRow(m_deviceTable->currentRow());
    });
    return page;
}
void MainWindow::populateDevices() {
    m_deviceTable->setRowCount(0);
    for (const auto &d : m_project.devices) {
        const int row = m_deviceTable->rowCount();
        m_deviceTable->insertRow(row);
        const QStringList cells{
            d.id,
            d.category,
            d.model,
            d.backend,
            d.host,
            QString::number(d.port),
            QString::number(d.unitId),
            QString::fromUtf8(QJsonDocument(d.parameters).toJson(QJsonDocument::Compact))};
        for (int col = 0; col < cells.size(); ++col)
            m_deviceTable->setItem(row, col, new QTableWidgetItem(cells[col]));
    }
}
void MainWindow::addDevice(const QString &category, const QString &backend) {
    if (m_running)
        return;
    DeviceConfig d;
    d.id = category + "-" + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    d.category = category;
    d.backend = backend;
    d.model = backend == "simulated" ? (category == "robot" ? "SCARA 四轴仿真机械臂" : "模拟相机")
                                     : "通用 Modbus TCP " + category;
    d.parameters = {{"timeoutMs", 1000}};
    const int row = m_deviceTable->rowCount();
    m_deviceTable->insertRow(row);
    const QStringList cells{d.id,
                            d.category,
                            d.model,
                            d.backend,
                            d.host,
                            QString::number(d.port),
                            QString::number(d.unitId),
                            "{\"timeoutMs\":1000}"};
    for (int col = 0; col < cells.size(); ++col)
        m_deviceTable->setItem(row, col, new QTableWidgetItem(cells[col]));
    m_deviceTable->selectRow(row);
}
QWidget *MainWindow::buildHistory() {
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 8, 24, 20);
    layout->addWidget(heading("历史检测与百万级查询"));
    layout->addWidget(hint("数据库游标分页，每页100条。基准库独立于生产数据；双击记录可查看保留的原图。"));
    auto *bar = new QHBoxLayout;
    m_historySource = choices({"当前检测数据库", "独立百万条基准库"});
    m_workpiece = new QLineEdit;
    m_workpiece->setPlaceholderText("精确工件编号");
    m_outcome = choices({"全部结果", "OK", "NG", "ERROR"});
    auto *query = button("查询", "primary");
    auto *exportButton = button("导出筛选结果 CSV");
    bar->addWidget(m_historySource);
    bar->addWidget(m_workpiece, 1);
    bar->addWidget(m_outcome);
    bar->addWidget(query);
    bar->addWidget(exportButton);
    layout->addLayout(bar);
    auto *dates = new QHBoxLayout;
    m_dateEnabled = new QCheckBox("按时间筛选");
    m_dateFrom = new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7));
    m_dateTo = new QDateTimeEdit(QDateTime::currentDateTime().addDays(1));
    for (auto *edit : {m_dateFrom, m_dateTo}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat("yyyy-MM-dd HH:mm:ss");
    }
    dates->addWidget(m_dateEnabled);
    dates->addWidget(m_dateFrom);
    dates->addWidget(new QLabel("至"));
    dates->addWidget(m_dateTo);
    dates->addStretch();
    layout->addLayout(dates);
    m_historyTable = recordView(m_historyModel);
    layout->addWidget(m_historyTable, 1);
    auto *paging = new QHBoxLayout;
    m_historyStatus = new QLabel("等待查询");
    m_previous = button("← 上一页");
    m_next = button("下一页 →");
    paging->addWidget(m_historyStatus, 1);
    paging->addWidget(m_previous);
    paging->addWidget(m_next);
    layout->addLayout(paging);
    auto *benchmark = new QGroupBox("独立容量验证");
    auto *benchLayout = new QHBoxLayout(benchmark);
    auto *seed = button("生成至 100 万条测试记录");
    auto *cancel = button("取消生成");
    m_seedStatus = hint("测试库：data/benchmark.sqlite · 不生成图像，不修改检测数据库");
    benchLayout->addWidget(seed);
    benchLayout->addWidget(cancel);
    benchLayout->addWidget(m_seedStatus, 1);
    layout->addWidget(benchmark);
    connect(query, &QPushButton::clicked, this, [this] { refreshHistory(); });
    connect(m_workpiece, &QLineEdit::returnPressed, this, [this] { refreshHistory(); });
    connect(m_historySource, &QComboBox::currentIndexChanged, this, [this] { refreshHistory(); });
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportHistory);
    connect(seed, &QPushButton::clicked, this, &MainWindow::seedBenchmark);
    connect(cancel, &QPushButton::clicked, this, [this] {
        if (m_seedCancel)
            m_seedCancel->store(true);
    });
    connect(m_next, &QPushButton::clicked, this, [this] {
        const auto rows = m_historyModel->records();
        if (rows.isEmpty())
            return;
        m_previousCursors.append(m_cursor);
        m_cursor = rows.last().id;
        refreshHistory(false);
    });
    connect(m_previous, &QPushButton::clicked, this, [this] {
        if (m_previousCursors.isEmpty())
            return;
        m_cursor = m_previousCursors.takeLast();
        refreshHistory(false);
    });
    connect(m_historyTable, &QTableView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto rows = m_historyModel->records();
        if (index.row() < 0 || index.row() >= rows.size())
            return;
        const auto path = rows[index.row()].imagePath;
        if (path.isEmpty()) {
            QMessageBox::information(this, "检测图像", "该记录未按策略保留图像。");
            return;
        }
        const auto absolute = QDir::isAbsolutePath(path) ? path : QDir(m_workspace).filePath(path);
        QImage image(absolute);
        if (image.isNull()) {
            QMessageBox::information(this, "检测图像", "图像已清理或文件不可读取；检测记录仍然保留。");
            return;
        }
        auto *dialog = new QDialog(this);
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(rows[index.row()].workpieceId);
        dialog->resize(900, 650);
        auto *l = new QVBoxLayout(dialog);
        auto *view = new ImageView;
        InspectionResult result;
        result.frame = image;
        result.record = rows[index.row()];
        view->setResult(result);
        l->addWidget(view);
        dialog->show();
    });
    return page;
}
QWidget *MainWindow::buildSettings() {
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(24, 8, 24, 20);
    layout->addWidget(heading("工程参数与部署"));
    auto *columns = new QHBoxLayout;
    auto *left = new QVBoxLayout;
    auto *right = new QVBoxLayout;
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);
    layout->addLayout(columns);
    auto *general = new QGroupBox("采集与工艺");
    auto *form = new QFormLayout(general);
    m_name = new QLineEdit;
    m_name->setObjectName("projectNameInput");
    m_recipe = new QLineEdit;
    m_resolutionPreset = choices({"640 × 480 · 调试", "1280 × 1024 · 1.3 MP", "1920 × 1200 · 2.3 MP",
                                  "2448 × 2048 · 5 MP", "4096 × 3000 · 12 MP", "自定义尺寸"});
    m_width = new QSpinBox;
    m_height = new QSpinBox;
    for (auto *b : {m_width, m_height})
        b->setRange(64, 8192);
    auto *sizeRow = new QWidget;
    auto *sizeLayout = new QHBoxLayout(sizeRow);
    sizeLayout->setContentsMargins(0, 0, 0, 0);
    sizeLayout->addWidget(m_width);
    sizeLayout->addWidget(new QLabel("×"));
    sizeLayout->addWidget(m_height);
    m_cycle = new QSpinBox;
    m_cycle->setRange(100, 60000);
    m_cycle->setSingleStep(100);
    m_cycle->setSuffix(" ms / 件");
    m_pixelsPerMm = decimal(20, .001);
    m_minDiameter = decimal(19);
    m_maxDiameter = decimal(21);
    form->addRow("工程名称", m_name);
    form->addRow("配方版本", m_recipe);
    form->addRow("分辨率预设", m_resolutionPreset);
    form->addRow("采集尺寸", sizeRow);
    form->addRow("目标节拍", m_cycle);
    form->addRow("标定比例 px/mm", m_pixelsPerMm);
    form->addRow("直径下限 mm", m_minDiameter);
    form->addRow("直径上限 mm", m_maxDiameter);
    left->addWidget(general);
    connect(m_resolutionPreset, &QComboBox::currentIndexChanged, this, [this](int i) {
        const QList<QSize> sizes{{640, 480}, {1280, 1024}, {1920, 1200}, {2448, 2048}, {4096, 3000}};
        if (i >= 0 && i < sizes.size()) {
            m_width->setValue(sizes[i].width());
            m_height->setValue(sizes[i].height());
        }
    });
    auto *images = new QGroupBox("图像保留");
    auto *imageForm = new QFormLayout(images);
    m_imagePolicy = choices({"仅保存 NG", "全部保存", "每10件抽样", "不保存图像"});
    m_retention = new QSpinBox;
    m_retention->setRange(1, 3650);
    m_retention->setSuffix(" 天");
    m_quota = new QSpinBox;
    m_quota->setRange(1, 1000000);
    m_quota->setSuffix(" MB");
    imageForm->addRow("保存策略", m_imagePolicy);
    imageForm->addRow("保留期限", m_retention);
    imageForm->addRow("容量限额", m_quota);
    imageForm->addRow(
        hint("图像独立于检测记录；清理图像后仍保留结果。模拟相机每第5件产生超差直径，用于验证NG链路。"));
    left->addWidget(images);
    left->addStretch();
    auto *db = new QGroupBox("数据库接入");
    auto *dbForm = new QFormLayout(db);
    m_dbDriver = choices({"QSQLITE", "QODBC"});
    m_dbPath = new QLineEdit;
    m_dbHost = new QLineEdit;
    m_dbPort = new QSpinBox;
    m_dbPort->setRange(1, 65535);
    m_dbUser = new QLineEdit;
    m_dbPasswordEnv = new QLineEdit;
    m_odbcDriver = new QLineEdit;
    m_trustServer = new QCheckBox("信任服务器证书（仅受控测试环境）");
    dbForm->addRow("后端", m_dbDriver);
    dbForm->addRow("文件 / 数据库名", m_dbPath);
    dbForm->addRow("SQL Server 主机", m_dbHost);
    dbForm->addRow("端口", m_dbPort);
    dbForm->addRow("用户", m_dbUser);
    dbForm->addRow("密码环境变量名", m_dbPasswordEnv);
    dbForm->addRow("ODBC 驱动", m_odbcDriver);
    dbForm->addRow(m_trustServer);
    dbForm->addRow(
        hint("SQLite 文件相对于工作目录。SQL Server 需安装对应ODBC驱动；密码从环境读取，不写入工程JSON。"));
    right->addWidget(db);
    auto *system = new QGroupBox("构建与支持状态");
    auto *systemLayout = new QVBoxLayout(system);
    systemLayout->addWidget(
        hint(QString("Qt %1 · %2 / %3\n%4\n%5\nSQL drivers: %6")
                 .arg(qVersion(), QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture(),
                      algorithmBackend(), halconStatus(), QSqlDatabase::drivers().join(", "))));
    auto *folder = button("打开工程文件夹");
    auto *publishButton = button("发布当前平台运行包", "primary");
    systemLayout->addWidget(folder);
    systemLayout->addWidget(publishButton);
    systemLayout->addWidget(hint(
        "发布会收集Qt运行依赖并生成干净的客户工程，不复制检测数据库和原图。当前主机只发布当前平台二进制。"));
    right->addWidget(system);
    right->addStretch();
    auto *apply = button("校验并应用工程参数", "primary");
    layout->addWidget(apply, 0, Qt::AlignLeft);
    layout->addWidget(hint("当前示范算法适用于暗背景单个亮色圆形工件。真实项目需用样本完成算法与标定验证；软"
                           "件停止不能代替硬件急停。"));
    layout->addStretch();
    scroll->setWidget(page);
    connect(folder, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_workspace)); });
    connect(publishButton, &QPushButton::clicked, this, &MainWindow::publish);
    connect(apply, &QPushButton::clicked, this, [this] {
        ProjectConfig p;
        QString error;
        if (!collectProject(&p, &error)) {
            reportError(error);
            return;
        }
        m_project = p;
        m_projectLabel->setText(p.name);
        rebuildDashboard();
        appendLog("INFO", "参数校验通过，已应用至下一次运行；请保存工程以持久化。");
    });
    return scroll;
}
void MainWindow::populateProject() {
    m_loading = true;
    m_name->setText(m_project.name);
    m_recipe->setText(m_project.recipe);
    m_width->setValue(m_project.resolution.width());
    m_height->setValue(m_project.resolution.height());
    const QList<QSize> sizes{{640, 480}, {1280, 1024}, {1920, 1200}, {2448, 2048}, {4096, 3000}};
    const int preset = sizes.indexOf(m_project.resolution);
    m_resolutionPreset->setCurrentIndex(preset < 0 ? 5 : preset);
    m_cycle->setValue(m_project.cycleMs);
    m_pixelsPerMm->setValue(m_project.pixelsPerMm);
    m_minDiameter->setValue(m_project.minDiameterMm);
    m_maxDiameter->setValue(m_project.maxDiameterMm);
    m_imagePolicy->setCurrentIndex(QStringList{"ng", "all", "sample", "none"}.indexOf(m_project.imagePolicy));
    m_retention->setValue(m_project.imageRetentionDays);
    m_quota->setValue(int(m_project.imageQuotaMB));
    m_dbDriver->setCurrentText(m_project.database.driver);
    m_dbPath->setText(m_project.database.database);
    m_dbHost->setText(m_project.database.host);
    m_dbPort->setValue(m_project.database.port);
    m_dbUser->setText(m_project.database.user);
    m_dbPasswordEnv->setText(m_project.database.passwordEnvironment);
    m_odbcDriver->setText(m_project.database.odbcDriver);
    m_trustServer->setChecked(m_project.database.trustServerCertificate);
    m_flowEditor->setNodes(m_project.flow);
    m_panelEditor->setPanels(m_project.panels);
    populateDevices();
    QString simulationError;
    if (!m_simulation->configure(m_project.devices, &simulationError))
        appendLog("ERROR", "仿真设备配置失败: " + simulationError);
    m_simulation->setPixelsPerMm(m_project.pixelsPerMm);
    m_simulationConsole->setDevices(m_project.devices);
    m_robotConsole->setDevices(m_project.devices);
    m_projectLabel->setText(m_project.name);
    rebuildDashboard();
    m_loading = false;
    m_dirty = false;
    setWindowModified(false);
}
bool MainWindow::collectProject(ProjectConfig *out, QString *error) const {
    ProjectConfig p = m_project;
    p.name = m_name->text().trimmed();
    p.recipe = m_recipe->text().trimmed();
    p.resolution = QSize(m_width->value(), m_height->value());
    p.cycleMs = m_cycle->value();
    p.pixelsPerMm = m_pixelsPerMm->value();
    p.minDiameterMm = m_minDiameter->value();
    p.maxDiameterMm = m_maxDiameter->value();
    p.imagePolicy = QStringList{"ng", "all", "sample", "none"}.value(m_imagePolicy->currentIndex(), "none");
    p.imageRetentionDays = m_retention->value();
    p.imageQuotaMB = m_quota->value();
    p.database.driver = m_dbDriver->currentText();
    p.database.database = m_dbPath->text().trimmed();
    p.database.host = m_dbHost->text().trimmed();
    p.database.port = m_dbPort->value();
    p.database.user = m_dbUser->text();
    p.database.passwordEnvironment = m_dbPasswordEnv->text().trimmed();
    p.database.odbcDriver = m_odbcDriver->text().trimmed();
    p.database.trustServerCertificate = m_trustServer->isChecked();
    p.flow = m_flowEditor->nodes();
    p.panels = m_panelEditor->panels();
    p.devices.clear();
    for (int row = 0; row < m_deviceTable->rowCount(); ++row) {
        QStringList cells;
        for (int col = 0; col < 8; ++col)
            cells.append(m_deviceTable->item(row, col) ? m_deviceTable->item(row, col)->text() : QString{});
        bool portOk = false, unitOk = false;
        const int port = cells[5].toInt(&portOk), unit = cells[6].toInt(&unitOk);
        QJsonParseError jsonError;
        const auto params = QJsonDocument::fromJson(cells[7].toUtf8(), &jsonError);
        if (!portOk || !unitOk || jsonError.error != QJsonParseError::NoError || !params.isObject()) {
            if (error)
                *error = QString("设备第%1行：端口、Unit ID或JSON参数无效").arg(row + 1);
            return false;
        }
        p.devices.append({cells[0].trimmed(), cells[1].trimmed(), cells[2], cells[3].trimmed(),
                          cells[4].trimmed(), port, unit, params.object()});
    }
    const auto errors = validateProject(p);
    if (!errors.isEmpty()) {
        if (error)
            *error = errors.join('\n');
        return false;
    }
    *out = p;
    if (error)
        error->clear();
    return true;
}
void MainWindow::newProject() {
    if (m_running || m_runtime || !confirmDiscardOrSave())
        return;
    m_project = defaultProject();
    m_simulation->configure({});
    m_simulation->reset();
    m_simulationConsole->resetEdits();
    m_robotConsole->resetEdits();
    m_projectPath.clear();
    m_recentRecords.clear();
    m_lastResult = {};
    m_liveModel->setRecords({});
    m_total = m_ok = 0;
    populateProject();
    m_totalValue->setText("0");
    m_yieldValue->setText("—");
    m_measureValue->setText("— mm");
    m_timeValue->setText("— ms");
    appendLog("INFO", "已创建新的示范工程；检测数据库未删除。");
}
void MainWindow::openProject() {
    if (m_running || m_runtime || !confirmDiscardOrSave())
        return;
    const auto path = QFileDialog::getOpenFileName(
        this, "打开可视化工程", QDir(m_workspace).filePath("examples"), "工业视觉工程 (*.ivproj *.json)");
    if (path.isEmpty())
        return;
    ProjectConfig p;
    QString error;
    if (!loadProject(path, &p, &error)) {
        reportError(error);
        return;
    }
    m_project = p;
    m_projectPath = path;
    m_simulation->configure({});
    m_simulation->reset();
    m_simulationConsole->resetEdits();
    m_robotConsole->resetEdits();
    populateProject();
    appendLog("INFO", "已打开工程: " + path);
    refreshHistory();
}
bool MainWindow::saveCurrentProject() {
    if (m_running || m_runtime)
        return false;
    ProjectConfig p;
    QString error;
    if (!collectProject(&p, &error)) {
        reportError(error);
        return false;
    }
    QString path = m_projectPath;
    if (path.isEmpty())
        path = QFileDialog::getSaveFileName(this, "保存工程",
                                            QDir(m_workspace).filePath("examples/my-project.ivproj"),
                                            "工业视觉工程 (*.ivproj)");
    if (path.isEmpty())
        return false;
    if (!saveProject(p, path, &error)) {
        reportError(error);
        return false;
    }
    m_project = p;
    m_projectPath = path;
    m_projectLabel->setText(p.name);
    m_dirty = false;
    setWindowModified(false);
    appendLog("INFO", "工程已保存: " + path);
    statusBar()->showMessage("工程已保存", 5000);
    return true;
}
bool MainWindow::confirmDiscardOrSave() {
    if (!m_dirty || m_runtime)
        return true;
    const auto answer = QMessageBox::question(this, "保存工程修改", "工程有未保存的修改。是否保存后继续？",
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                                              QMessageBox::Save);
    if (answer == QMessageBox::Cancel)
        return false;
    if (answer == QMessageBox::Discard)
        return true;
    if (m_running) {
        QMetaObject::invokeMethod(m_worker, &PipelineWorker::stop, Qt::BlockingQueuedConnection);
        setRunState("stopped");
    }
    return saveCurrentProject();
}
void MainWindow::startRun(bool single) {
    if (m_running)
        return;
    ProjectConfig p;
    QString error;
    if (!collectProject(&p, &error)) {
        reportError("启动前校验失败:\n" + error);
        return;
    }
    if (!m_simulation->configure(p.devices, &error) || !m_simulation->setPixelsPerMm(p.pixelsPerMm, &error)) {
        reportError("仿真配置失败:\n" + error);
        return;
    }
    m_simulationConsole->setDevices(p.devices);
    m_robotConsole->setDevices(p.devices);
    m_project = p;
    m_projectLabel->setText(p.name);
    m_total = m_ok = 0;
    m_recentRecords.clear();
    m_liveModel->setRecords({});
    for (auto *chart : m_charts)
        chart->clear();
    m_totalValue->setText("0");
    m_yieldValue->setText("—");
    setRunState("running");
    QMetaObject::invokeMethod(
        m_worker,
        [worker = m_worker, p, workspace = m_workspace, single] { worker->start(p, workspace, single); },
        Qt::QueuedConnection);
}
void MainWindow::stopRun() {
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, &PipelineWorker::stop, Qt::QueuedConnection);
}
void MainWindow::setRunState(const QString &state) {
    m_running = state == "running";
    m_simulationConsole->setRunning(m_running);
    m_robotConsole->setRunning(m_running);
    const QString text = m_running            ? "●  运行中"
                         : state == "fault"   ? "●  故障"
                         : state == "stopped" ? "●  已停止"
                                              : "●  就绪";
    m_status->setText(text);
    m_status->setStyleSheet(
        state == "fault" ? "background:#fce9ed;color:#b53d54;padding:8px 16px;border-radius:16px;" : "");
    m_start->setEnabled(!m_running);
    m_single->setEnabled(!m_running);
    m_stop->setEnabled(m_running);
    for (auto *b : {m_new, m_open, m_save, m_publish})
        b->setEnabled(!m_running && !m_publishBusy);
    m_flowEditor->setEditingEnabled(!m_running && !m_runtime);
    m_deviceTable->setEnabled(!m_running && !m_runtime);
    m_panelEditor->setEnabled(!m_running && !m_runtime);
    m_pages->widget(5)->setEnabled(!m_running && !m_runtime);
    for (int i : {1, 2, 3, 5})
        m_navigation->item(i)->setFlags(m_running ? m_navigation->item(i)->flags() & ~Qt::ItemIsEnabled
                                                  : m_navigation->item(i)->flags() | Qt::ItemIsEnabled);
    if (!m_running)
        statusBar()->showMessage(state == "fault" ? "流程故障：查看运行日志，修正后可重新启动"
                                                  : "设备已停止，可编辑工程");
}
void MainWindow::receiveResult(const InspectionResult &result) {
    m_lastResult = result;
    ++m_total;
    if (result.record.outcome == "OK")
        ++m_ok;
    m_totalValue->setText(QString::number(m_total));
    m_yieldValue->setText(QString::number(100.0 * m_ok / qMax<qint64>(1, m_total), 'f', 1) + " %");
    m_measureValue->setText(QString::number(result.record.diameterMm, 'f', 3) + " mm");
    m_timeValue->setText(QString::number(result.record.durationMs, 'f', 1) + " ms");
    m_liveModel->prependRecord(result.record);
    m_recentRecords.append(result.record);
    if (m_recentRecords.size() > 1000)
        m_recentRecords.removeFirst();
    for (auto *view : m_imageViews)
        view->setResult(result);
    for (auto *chart : m_charts)
        chart->appendRecord(result.record);
}
void MainWindow::appendLog(const QString &level, const QString &message) {
    const auto line =
        QString("%1  [%2] %3").arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"), level, message);
    m_logLines.append(line);
    while (m_logLines.size() > 500)
        m_logLines.removeFirst();
    for (auto *log : m_logViews)
        log->appendPlainText(line);
    const QString directory = QDir(m_workspace).filePath("data/logs");
    QDir().mkpath(directory);
    QFile file(QDir(directory).filePath(QDate::currentDate().toString("yyyy-MM-dd") + ".log"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append))
        file.write((line + '\n').toUtf8());
}
void MainWindow::reportError(const QString &message) {
    appendLog("ERROR", message);
    QMessageBox::warning(this, "操作未完成", message);
}
DatabaseConfig MainWindow::selectedHistoryDatabase() const {
    if (m_historySource->currentIndex() == 1) {
        DatabaseConfig config;
        config.database = "data/benchmark.sqlite";
        return config;
    }
    return m_project.database;
}
RecordFilter MainWindow::historyFilter() const {
    RecordFilter f;
    f.workpiece = m_workpiece->text().trimmed();
    f.outcome = m_outcome->currentIndex() == 0 ? QString{} : m_outcome->currentText();
    f.beforeId = m_cursor;
    f.limit = 100;
    if (m_dateEnabled->isChecked()) {
        f.from = m_dateFrom->dateTime();
        f.to = m_dateTo->dateTime();
    }
    return f;
}
void MainWindow::refreshHistory(bool resetCursor) {
    if (resetCursor) {
        m_cursor = 0;
        m_previousCursors.clear();
    }
    const auto filter = historyFilter();
    if (filter.from.isValid() && filter.to.isValid() && filter.from > filter.to) {
        reportError("开始时间不能晚于结束时间。");
        return;
    }
    const auto generation = ++m_historyGeneration;
    const auto config = selectedHistoryDatabase();
    const auto workspace = m_workspace;
    m_historyBusy = true;
    m_previous->setEnabled(false);
    m_next->setEnabled(false);
    m_historyStatus->setText("正在后台查询…");
    auto *watcher = new QFutureWatcher<HistoryPage>(this);
    connect(watcher, &QFutureWatcher<HistoryPage>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != m_historyGeneration)
            return;
        m_historyBusy = false;
        if (!result.error.isEmpty()) {
            m_historyModel->setRecords({});
            m_historyStatus->setText("查询失败: " + result.error);
            appendLog("ERROR", result.error);
            return;
        }
        m_historyModel->setRecords(result.records);
        m_historyStatus->setText(QString("匹配 %1 条 · 本页 %2 条 · 查询 %3 ms · 第 %4 页")
                                     .arg(result.count)
                                     .arg(result.records.size())
                                     .arg(result.elapsed)
                                     .arg(m_previousCursors.size() + 1));
        m_previous->setEnabled(!m_previousCursors.isEmpty());
        m_next->setEnabled(result.records.size() == 100);
    });
    watcher->setFuture(QtConcurrent::run([config, workspace, filter] {
        HistoryPage result;
        QElapsedTimer timer;
        timer.start();
        RecordRepository repository;
        if (!repository.open(config, workspace, &result.error))
            return result;
        result.records = repository.query(filter, &result.error);
        if (!result.error.isEmpty())
            return result;
        auto all = filter;
        all.beforeId = 0;
        result.count = repository.count(all, &result.error);
        result.elapsed = timer.elapsed();
        return result;
    }));
}
void MainWindow::exportHistory() {
    const auto path = QFileDialog::getSaveFileName(
        this, "导出全部筛选记录", QDir(m_workspace).filePath("reports/inspection-export.csv"), "CSV (*.csv)");
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    auto filter = historyFilter();
    filter.beforeId = 0;
    const auto config = selectedHistoryDatabase();
    const auto workspace = m_workspace;
    appendLog("INFO", "开始后台导出CSV…");
    auto *watcher = new QFutureWatcher<JobResult>(this);
    connect(watcher, &QFutureWatcher<JobResult>::finished, this, [this, watcher, path] {
        const auto r = watcher->result();
        watcher->deleteLater();
        if (!r.success) {
            reportError(r.error);
            return;
        }
        appendLog("INFO", QString("已导出 %1 条记录: %2").arg(r.count).arg(path));
        QMessageBox::information(this, "导出完成", QString("已导出 %1 条记录。\n%2").arg(r.count).arg(path));
    });
    watcher->setFuture(QtConcurrent::run([config, workspace, filter, path] {
        JobResult r;
        RecordRepository repo;
        if (!repo.open(config, workspace, &r.error))
            return r;
        r.success = repo.exportCsv(path, filter, &r.error);
        if (r.success)
            r.count = repo.count(filter, &r.error);
        return r;
    }));
}
void MainWindow::seedBenchmark() {
    if (m_seedBusy)
        return;
    m_seedBusy = true;
    m_seedCancel = std::make_shared<std::atomic_bool>(false);
    m_seedStatus->setText("正在后台生成（目标100万条）；可取消，不影响检测数据库…");
    const auto cancel = m_seedCancel;
    const auto workspace = m_workspace;
    auto *watcher = new QFutureWatcher<JobResult>(this);
    connect(watcher, &QFutureWatcher<JobResult>::finished, this, [this, watcher] {
        const auto r = watcher->result();
        watcher->deleteLater();
        m_seedBusy = false;
        m_seedStatus->setText(
            QString("基准库 %1 条 · %2").arg(r.count).arg(r.success ? "生成完成" : r.error));
        appendLog(r.success ? "INFO" : "WARN", m_seedStatus->text());
        if (r.success) {
            m_historySource->setCurrentIndex(1);
            refreshHistory();
        }
    });
    watcher->setFuture(QtConcurrent::run([workspace, cancel] {
        JobResult r;
        DatabaseConfig config;
        config.database = "data/benchmark.sqlite";
        RecordRepository repo;
        if (!repo.open(config, workspace, &r.error))
            return r;
        const auto current = repo.count({}, &r.error);
        if (!r.error.isEmpty())
            return r;
        r.success = repo.seed(qMax<qint64>(0, 1000000 - current), &r.error, {}, cancel.get());
        QString countError;
        r.count = repo.count({}, &countError);
        return r;
    }));
}
void MainWindow::publish() {
    if (m_running || m_publishBusy || m_runtime)
        return;
    ProjectConfig p;
    QString error;
    if (!collectProject(&p, &error)) {
        reportError(error);
        return;
    }
    const auto parent =
        QFileDialog::getExistingDirectory(this, "选择发布目录（会创建独立子目录）", m_workspace);
    if (parent.isEmpty())
        return;
    const auto destination = QDir(parent).filePath(
        "VisionRuntime-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz"));
    m_publishBusy = true;
    m_publish->setEnabled(false);
    statusBar()->showMessage("正在收集Qt依赖并发布运行包…");
    appendLog("INFO", "正在发布: " + destination);
    const auto executable = QCoreApplication::applicationFilePath();
    auto *watcher = new QFutureWatcher<PublishResult>(this);
    connect(watcher, &QFutureWatcher<PublishResult>::finished, this, [this, watcher] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_publishBusy = false;
        m_publish->setEnabled(true);
        if (!result.success) {
            reportError(result.error);
            return;
        }
        appendLog("INFO", "运行包已发布: " + result.directory);
        statusBar()->showMessage("发布成功 · " + result.directory);
        QMessageBox::information(
            this, "发布完成", "运行包已生成：\n" + result.directory + "\n\n启动入口：\n" + result.launcher);
    });
    watcher->setFuture(QtConcurrent::run(
        [p, destination, executable] { return publishRuntime(p, destination, executable); }));
}
} // namespace ivs
