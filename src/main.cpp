#include "application/PipelineWorker.h"
#include "application/Publisher.h"
#include "domain/Project.h"
#include "infrastructure/RecordRepository.h"
#include "infrastructure/SimulationPlant.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTextStream>
#include <QThread>
#include <QTimer>

namespace {
bool writeJson(const QString &path, const QJsonObject &object) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
QString adjacentDirectory() {
#ifdef Q_OS_MACOS
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp();
    dir.cdUp();
    dir.cdUp();
    return dir.absolutePath();
#else
    return QCoreApplication::applicationDirPath();
#endif
}
int smoke(QApplication &app, ivs::ProjectConfig project, const QString &workspace, int cycles) {
    QDir().mkpath(workspace);
    project.resolution = QSize(640, 480);
    project.cycleMs = 100;
    project.imagePolicy = "ng";
    project.database = ivs::DatabaseConfig{}; // smoke is deliberately isolated from production/server data
    QString error;
    if (!ivs::saveProject(project, QDir(workspace).filePath("smoke.ivproj"), &error)) {
        QTextStream(stderr) << error << '\n';
        return 2;
    }
    qint64 before = 0;
    {
        ivs::RecordRepository repo;
        if (!repo.open(project.database, workspace, &error)) {
            QTextStream(stderr) << error << '\n';
            return 2;
        }
        before = repo.count({}, &error);
    }
    QThread thread;
    auto *worker = new ivs::PipelineWorker;
    auto plant = std::make_shared<ivs::SimulationPlant>();
    worker->setSimulationPlant(plant);
    worker->moveToThread(&thread);
    QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
    QJsonArray records;
    QStringList logs;
    bool failed = false;
    int received = 0, completed = 0;
    QObject::connect(
        worker, &ivs::PipelineWorker::logMessage, &app,
        [&](const QString &level, const QString &message) { logs.append(level + ": " + message); });
    QObject::connect(worker, &ivs::PipelineWorker::resultReady, &app,
                     [&](const ivs::InspectionResult &result) {
                         ++received;
                         records.append(QJsonObject{{"id", double(result.record.id)},
                                                    {"outcome", result.record.outcome},
                                                    {"diameterMm", result.record.diameterMm},
                                                    {"imagePath", result.record.imagePath}});
                     });
    QObject::connect(worker, &ivs::PipelineWorker::cycleFinished, &app, [&] { ++completed; });
    // Stop at the worker's cycle boundary before it can schedule the next cycle.
    // Keep the app-thread report counter separate from this worker-thread counter.
    int workerCompleted = 0;
    QObject::connect(worker, &ivs::PipelineWorker::cycleFinished, worker, [&] {
        if (++workerCompleted >= cycles)
            worker->stop();
    });
    QObject::connect(worker, &ivs::PipelineWorker::stateChanged, &app, [&](const QString &state) {
        if (state == "fault") {
            failed = true;
            app.exit(3);
        } else if (state == "stopped" && completed >= cycles)
            app.exit(0);
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &app, [&] {
        failed = true;
        logs << "watchdog timeout";
        app.exit(4);
    });
    timeout.start(qMax(30000, cycles * 30000));
    thread.start();
    QMetaObject::invokeMethod(
        worker, [=] { worker->start(project, workspace, false); }, Qt::QueuedConnection);
    const int result = app.exec();
    QMetaObject::invokeMethod(worker, &ivs::PipelineWorker::shutdown, Qt::BlockingQueuedConnection);
    thread.quit();
    thread.wait();
    qint64 after = -1;
    {
        ivs::RecordRepository repo;
        if (repo.open(project.database, workspace, &error))
            after = repo.count({}, &error);
    }
    QJsonArray robots;
    for (const auto &device : plant->snapshot().devices)
        if (device.category == "robot")
            robots.append(QJsonObject{{"id", device.id},
                                      {"okCount", double(device.robot.okCount)},
                                      {"ngCount", double(device.robot.ngCount)},
                                      {"partLocation", device.robot.partLocation},
                                      {"moving", device.robot.moving},
                                      {"programRunning", device.robot.programRunning}});
    const bool success =
        !failed && result == 0 && received == cycles && completed == cycles && after - before == cycles;
    const QJsonObject report{{"success", success}, {"qtVersion", qVersion()},
                             {"cycles", received}, {"completedCycles", completed},
                             {"robots", robots},   {"rowsAdded", double(after - before)},
                             {"records", records}, {"logs", QJsonArray::fromStringList(logs)},
                             {"error", error}};
    if (!writeJson(QDir(workspace).filePath("reports/smoke.json"), report))
        return 5;
    QTextStream(stdout) << QJsonDocument(report).toJson(QJsonDocument::Compact) << '\n';
    return success ? 0 : 3;
}
} // namespace
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("IndustrialVisionStudio");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("IndustrialVision");
    QApplication::setStyle("Fusion");
    QCommandLineParser parser;
    parser.setApplicationDescription("工业视觉通用软件框架 / Industrial Vision Studio");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({{"p", "project"}, "Open a versioned .ivproj project", "file"});
    parser.addOption({"workspace", "Directory for all data/logs/reports", "directory"});
    parser.addOption({"runtime", "Lock the application to customer runtime mode"});
    parser.addOption({"smoke-test", "Run isolated simulated acquisition/analysis/storage and exit"});
    parser.addOption({"cycles", "Smoke test cycle count", "count", "6"});
    parser.addOption({"write-example", "Write the built-in example project and exit", "file"});
    parser.addOption(
        {"publish-to", "Publish current-platform runtime into a NEW directory and exit", "directory"});
    parser.addOption({"auto-start", "Start continuous inspection when the window opens"});
    parser.addOption({"simulation-console", "Open the virtual hardware console on startup"});
    parser.addOption({"robot-console", "Open the SCARA simulation laboratory on startup"});
    parser.addOption({"screenshot", "Save a window screenshot after 2500 ms", "file"});
    parser.addOption({"quit-after", "Exit after a number of milliseconds (UI verification)", "ms"});
    parser.process(app);
    QString projectPath = parser.value("project");
    bool runtime = parser.isSet("runtime");
    const auto adjacent = adjacentDirectory();
    if (projectPath.isEmpty() && QFileInfo::exists(QDir(adjacent).filePath("project.ivproj"))) {
        projectPath = QDir(adjacent).filePath("project.ivproj");
        runtime = true;
    }
    QString workspace = parser.value("workspace");
    if (workspace.isEmpty()) {
        if (runtime && !projectPath.isEmpty())
            workspace = QFileInfo(projectPath).absolutePath();
        else
            workspace = QString::fromUtf8(IVS_SOURCE_DIR);
    }
    workspace = QFileInfo(workspace).absoluteFilePath();
    ivs::ProjectConfig project = ivs::defaultProject();
    QString error;
    if (!projectPath.isEmpty() && !ivs::loadProject(projectPath, &project, &error)) {
        QTextStream(stderr) << error << '\n';
        if (!parser.isSet("smoke-test") && !parser.isSet("publish-to"))
            QMessageBox::critical(nullptr, "工程读取失败", error);
        return 2;
    }
    if (parser.isSet("write-example"))
        return ivs::saveProject(project, parser.value("write-example"), &error) ? 0 : 2;
    if (parser.isSet("smoke-test")) {
        bool ok = false;
        const int count = parser.value("cycles").toInt(&ok);
        if (!ok || count < 1 || count > 100)
            return 2;
        return smoke(app, project, workspace, count);
    }
    if (parser.isSet("publish-to")) {
        const auto result =
            ivs::publishRuntime(project, parser.value("publish-to"), QCoreApplication::applicationFilePath());
        const QJsonObject json{{"success", result.success},
                               {"directory", result.directory},
                               {"launcher", result.launcher},
                               {"error", result.error}};
        QTextStream(stdout) << QJsonDocument(json).toJson(QJsonDocument::Indented);
        return result.success ? 0 : 3;
    }
    try {
        ivs::MainWindow window(workspace, projectPath, runtime);
        window.show();
        if (parser.isSet("simulation-console"))
            if (auto *navigation = window.findChild<QListWidget *>("navigation"))
                navigation->setCurrentRow(6);
        if (parser.isSet("robot-console"))
            if (auto *navigation = window.findChild<QListWidget *>("navigation"))
                navigation->setCurrentRow(7);
        if (parser.isSet("auto-start"))
            QTimer::singleShot(300, &window, [&window] {
                if (auto *start = window.findChild<QPushButton *>("startRunButton"))
                    start->click();
            });
        if (parser.isSet("screenshot")) {
            const auto path = parser.value("screenshot");
            QTimer::singleShot(2500, &window, [&window, path] {
                QDir().mkpath(QFileInfo(path).absolutePath());
                window.grab().save(path);
            });
        }
        if (parser.isSet("quit-after")) {
            bool ok = false;
            const int ms = parser.value("quit-after").toInt(&ok);
            if (!ok || ms < 1)
                return 2;
            QTimer::singleShot(ms, &window, &QWidget::close);
        }
        return app.exec();
    } catch (const std::exception &e) {
        QTextStream(stderr) << e.what() << '\n';
        return 2;
    }
}
