#pragma once
#include "domain/Types.h"
#include <QMainWindow>
#include <QThread>
#include <atomic>
#include <memory>

class QLabel;
class QPushButton;
class QListWidget;
class QStackedWidget;
class QGridLayout;
class QScrollArea;
class QTableView;
class QTableWidget;
class QPlainTextEdit;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QDoubleSpinBox;
class QDateTimeEdit;
class QCheckBox;

namespace ivs {
class PipelineWorker;
class FlowEditor;
class PanelEditor;
class ImageView;
class ChartWidget;
class RecordTableModel;
class SimulationPlant;
class SimulationConsole;
class RobotConsole;

class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    MainWindow(const QString &workspace, const QString &projectPath, bool runtimeMode,
               QWidget *parent = nullptr);
    ~MainWindow() override;

  protected:
    void closeEvent(QCloseEvent *event) override;

  private:
    void buildUi();
    QWidget *buildDashboard();
    QWidget *buildDevices();
    QWidget *buildHistory();
    QWidget *buildSettings();
    void populateProject();
    void populateDevices();
    void rebuildDashboard();
    bool collectProject(ProjectConfig *project, QString *error) const;
    void newProject();
    void openProject();
    bool saveCurrentProject();
    bool confirmDiscardOrSave();
    void startRun(bool single);
    void stopRun();
    void setRunState(const QString &state);
    void receiveResult(const InspectionResult &result);
    void appendLog(const QString &level, const QString &message);
    void reportError(const QString &message);
    void refreshHistory(bool resetCursor = true);
    void exportHistory();
    void seedBenchmark();
    void publish();
    void addDevice(const QString &category, const QString &backend);
    DatabaseConfig selectedHistoryDatabase() const;
    RecordFilter historyFilter() const;
    QString m_workspace, m_projectPath;
    ProjectConfig m_project;
    bool m_runtime = false, m_running = false, m_loading = false, m_historyBusy = false, m_seedBusy = false,
         m_publishBusy = false, m_dirty = false;
    quint64 m_historyGeneration = 0;
    qint64 m_total = 0, m_ok = 0, m_cursor = 0;
    QList<qint64> m_previousCursors;
    QList<DetectionRecord> m_recentRecords;
    InspectionResult m_lastResult;
    QStringList m_logLines;
    std::shared_ptr<std::atomic_bool> m_seedCancel;
    std::shared_ptr<SimulationPlant> m_simulation;
    SimulationConsole *m_simulationConsole = nullptr;
    RobotConsole *m_robotConsole = nullptr;
    QThread m_pipelineThread;
    PipelineWorker *m_worker = nullptr;
    QListWidget *m_navigation = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_status = nullptr, *m_projectLabel = nullptr, *m_totalValue = nullptr, *m_yieldValue = nullptr,
           *m_measureValue = nullptr, *m_timeValue = nullptr, *m_historyStatus = nullptr,
           *m_seedStatus = nullptr;
    QPushButton *m_start = nullptr, *m_single = nullptr, *m_stop = nullptr, *m_save = nullptr,
                *m_open = nullptr, *m_new = nullptr, *m_publish = nullptr, *m_previous = nullptr,
                *m_next = nullptr;
    QScrollArea *m_dashboardScroll = nullptr;
    QList<ImageView *> m_imageViews;
    QList<ChartWidget *> m_charts;
    QList<QPlainTextEdit *> m_logViews;
    RecordTableModel *m_liveModel = nullptr, *m_historyModel = nullptr;
    FlowEditor *m_flowEditor = nullptr;
    PanelEditor *m_panelEditor = nullptr;
    QTableWidget *m_deviceTable = nullptr;
    QTableView *m_historyTable = nullptr;
    QComboBox *m_historySource = nullptr, *m_outcome = nullptr;
    QLineEdit *m_workpiece = nullptr;
    QCheckBox *m_dateEnabled = nullptr;
    QDateTimeEdit *m_dateFrom = nullptr, *m_dateTo = nullptr;
    QLineEdit *m_name = nullptr, *m_recipe = nullptr, *m_dbPath = nullptr, *m_dbHost = nullptr,
              *m_dbUser = nullptr, *m_dbPasswordEnv = nullptr, *m_odbcDriver = nullptr;
    QComboBox *m_resolutionPreset = nullptr, *m_imagePolicy = nullptr, *m_dbDriver = nullptr;
    QSpinBox *m_width = nullptr, *m_height = nullptr, *m_cycle = nullptr, *m_retention = nullptr,
             *m_quota = nullptr, *m_dbPort = nullptr;
    QDoubleSpinBox *m_pixelsPerMm = nullptr, *m_minDiameter = nullptr, *m_maxDiameter = nullptr;
    QCheckBox *m_trustServer = nullptr;
};
} // namespace ivs
