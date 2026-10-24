#include "PipelineWorker.h"

#include "domain/ImageAnalysis.h"
#include "domain/Project.h"
#include "infrastructure/Devices.h"
#include "infrastructure/RecordRepository.h"
#include "infrastructure/SimulationPlant.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>

namespace ivs {

namespace {
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}

bool simulationReady(const SimulationSnapshot &snapshot, QString *error) {
    if (snapshot.emergencyStop)
        return fail(error, QStringLiteral("Simulation emergency stop is active"));
    for (const auto &device : snapshot.devices)
        if (!device.connected)
            return fail(error, QStringLiteral("Simulated device '%1' is disconnected").arg(device.id));
    return true;
}

bool timingParameter(const FlowNode &node, const QString &key, int fallback, int maximum, int *value,
                     QString *error) {
    const auto field = node.parameters.value(key);
    if (field.isUndefined()) {
        *value = fallback;
        return true;
    }
    const double number = field.toDouble(-1);
    if (!field.isDouble() || !std::isfinite(number) || number != std::floor(number) || number < 1 ||
        number > maximum)
        return fail(error, QStringLiteral("Node '%1' parameter '%2' must be an integer in 1..%3")
                               .arg(node.title, key)
                               .arg(maximum));
    *value = int(number);
    return true;
}

bool ensureImageDirectory(const QString &workspace, QString *directory, QString *error) {
    QString path = workspace;
    for (const auto &part :
         QStringList{QStringLiteral("data"), QStringLiteral("images"), QStringLiteral("ivs-managed")}) {
        path = QDir(path).filePath(part);
        const QFileInfo info(path);
        if (info.isSymLink() || (info.exists() && !info.isDir()))
            return fail(error,
                        QStringLiteral("Image directory must be a real directory without symbolic links: %1")
                            .arg(path));
        if (!QDir().mkpath(path))
            return fail(error, QStringLiteral("Cannot create image directory: %1").arg(path));
    }
    const QString canonical = QFileInfo(path).canonicalFilePath();
    const QString prefix = workspace.endsWith(QLatin1Char('/')) ? workspace : workspace + QLatin1Char('/');
#ifdef Q_OS_WIN
    const bool contained = canonical.startsWith(prefix, Qt::CaseInsensitive);
#else
    const bool contained = canonical.startsWith(prefix);
#endif
    if (!contained)
        return fail(error, QStringLiteral("Managed image directory is outside the workspace"));
    *directory = canonical;
    return true;
}

bool pruneImages(const QString &directory, int retentionDays, qint64 quotaMB, const QString &protectedPath,
                 QString *error) {
    const QRegularExpression managed(QStringLiteral("^ivs-[0-9a-f]{32}\\.png$"));
    auto entries = QDir(directory).entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Time | QDir::Reversed);
    QList<QFileInfo> retained;
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addDays(-retentionDays);
    qint64 total = 0;
    for (const auto &entry : entries) {
        if (!managed.match(entry.fileName()).hasMatch())
            continue;
        if (entry.absoluteFilePath() != protectedPath && entry.lastModified().toUTC() < cutoff) {
            if (!QFile::remove(entry.absoluteFilePath()))
                return fail(
                    error,
                    QStringLiteral("Cannot remove expired managed image: %1").arg(entry.absoluteFilePath()));
        } else {
            retained.append(entry);
            total += entry.size();
        }
    }
    const qint64 quota = quotaMB * 1024 * 1024;
    for (const auto &entry : retained) {
        if (total <= quota)
            break;
        if (entry.absoluteFilePath() == protectedPath)
            continue;
        if (!QFile::remove(entry.absoluteFilePath()))
            return fail(error, QStringLiteral("Cannot prune managed image to enforce quota: %1")
                                   .arg(entry.absoluteFilePath()));
        total -= entry.size();
    }
    if (total > quota)
        return fail(error, QStringLiteral("Current image exceeds the configured image quota"));
    return true;
}
} // namespace

struct PipelineWorker::Impl {
    explicit Impl(PipelineWorker *owner) : timer(new QTimer(owner)) {
        timer->setSingleShot(true);
        timer->setTimerType(Qt::PreciseTimer);
    }
    QTimer *timer;
    QMetaObject::Connection timerConnection;
    quint64 generation = 0;
    bool running = false;
    bool singleShot = false;
    bool shuttingDown = false;
    bool usesSimulation = false;
    quint64 interruptionGeneration = 0;
    quint64 sequence = 0;
    ProjectConfig project;
    QString workspace;
    QString managedImages;
    QString pendingImage;
    std::shared_ptr<SimulationPlant> plant = std::make_shared<SimulationPlant>();
    std::unique_ptr<DeviceGateway> gateway;
    std::unique_ptr<RecordRepository> repository;
    int index = 0;
    int triggerIndex = -1;
    int motionIndex = -1;
    int robotIndex = -1;
    QElapsedTimer cycleTime;
    QElapsedTimer triggerTime;
    QElapsedTimer motionTime;
    QElapsedTimer robotTime;
    InspectionResult current;
    bool captured = false;
    bool inspected = false;
    bool judged = false;
    bool stored = false;

    bool simulationHealthy(QString *error) const {
        if (!usesSimulation)
            return true;
        const auto snapshot = plant->snapshot();
        if (snapshot.interruptionGeneration != interruptionGeneration)
            return fail(error, QStringLiteral("Virtual hardware was interrupted; restart inspection"));
        return simulationReady(snapshot, error);
    }

    void clearCycle() {
        current = {};
        captured = inspected = judged = stored = false;
        index = 0;
        triggerIndex = -1;
        motionIndex = -1;
        robotIndex = -1;
    }
    void cancel(bool stopMotion = true) {
        ++generation;
        running = false;
        usesSimulation = false;
        timer->stop();
        QObject::disconnect(timerConnection);
        if (stopMotion && plant)
            plant->stopAllAxes();
        gateway.reset();
        repository.reset();
        if (!pendingImage.isEmpty())
            QFile::remove(pendingImage);
        pendingImage.clear();
        clearCycle();
    }
    void schedule(PipelineWorker *owner, int delay, std::function<void()> action) {
        if (!running || shuttingDown)
            return;
        timer->stop();
        QObject::disconnect(timerConnection);
        const quint64 token = generation;
        timerConnection =
            QObject::connect(timer, &QTimer::timeout, owner, [this, token, action = std::move(action)] {
                if (running && !shuttingDown && token == generation)
                    action();
            });
        timer->start(qMax(0, delay));
    }
};

PipelineWorker::PipelineWorker(QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {
    qRegisterMetaType<ivs::ProjectConfig>();
    qRegisterMetaType<ivs::InspectionResult>();
}

PipelineWorker::~PipelineWorker() {
    d->cancel();
}

bool PipelineWorker::setSimulationPlant(std::shared_ptr<SimulationPlant> plant) {
    if (QThread::currentThread() != thread() || d->running || d->shuttingDown)
        return false;
    d->plant = plant ? std::move(plant) : std::make_shared<SimulationPlant>();
    return true;
}

void PipelineWorker::start(const ProjectConfig &project, const QString &workspace, bool singleShot) {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(
            this, [this, project, workspace, singleShot] { start(project, workspace, singleShot); },
            Qt::QueuedConnection);
        return;
    }
    if (d->shuttingDown) {
        emit logMessage(QStringLiteral("error"), QStringLiteral("Worker has been shut down"));
        return;
    }
    if (d->running) {
        emit logMessage(QStringLiteral("warning"), QStringLiteral("Inspection is already running"));
        return;
    }
    const auto validation = validateProject(project);
    if (!validation.isEmpty()) {
        fault(QStringLiteral("Invalid project: ") + validation.join(QLatin1Char('\n')));
        return;
    }
    if (workspace.trimmed().isEmpty()) {
        fault(QStringLiteral("Workspace path is empty"));
        return;
    }
    d->cancel(false);
    if (!QDir().mkpath(workspace)) {
        fault(QStringLiteral("Cannot create workspace directory"));
        return;
    }
    d->workspace = QFileInfo(workspace).canonicalFilePath();
    if (d->workspace.isEmpty()) {
        fault(QStringLiteral("Cannot resolve workspace directory"));
        return;
    }
    d->project = project;
    d->singleShot = singleShot;
    d->sequence = 0;
    d->managedImages.clear();
    QString error;
    d->gateway = std::make_unique<DeviceGateway>(d->plant);
    if (!d->plant->setPixelsPerMm(project.pixelsPerMm, &error) ||
        !d->gateway->configure(project.devices, &error)) {
        fault(QStringLiteral("Device initialization failed: ") + error);
        return;
    }
    const auto simulation = d->plant->snapshot();
    d->usesSimulation = !simulation.devices.isEmpty();
    d->interruptionGeneration = simulation.interruptionGeneration;
    if ((d->usesSimulation && !simulationReady(simulation, &error)) || !d->gateway->connectAll(&error) ||
        !d->simulationHealthy(&error)) {
        fault(QStringLiteral("Device initialization failed: ") + error);
        return;
    }
    d->repository = std::make_unique<RecordRepository>();
    if (!d->repository->open(project.database, d->workspace, &error)) {
        fault(QStringLiteral("Database initialization failed: ") + error);
        return;
    }
    if (!d->simulationHealthy(&error)) {
        fault(QStringLiteral("Device initialization failed: ") + error);
        return;
    }
    d->running = true;
    emit stateChanged(QStringLiteral("running"));
    emit logMessage(QStringLiteral("info"), QStringLiteral("Inspection started: %1").arg(project.name));
    d->schedule(this, 0, [this] { beginCycle(); });
}

void PipelineWorker::stop() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, &PipelineWorker::stop, Qt::QueuedConnection);
        return;
    }
    d->cancel();
    emit stateChanged(QStringLiteral("stopped"));
    emit logMessage(QStringLiteral("info"), QStringLiteral("Inspection stopped"));
}

void PipelineWorker::shutdown() {
    if (QThread::currentThread() != thread()) {
        QMetaObject::invokeMethod(this, &PipelineWorker::shutdown, Qt::QueuedConnection);
        return;
    }
    d->shuttingDown = true;
    d->cancel();
    emit stateChanged(QStringLiteral("idle"));
}

void PipelineWorker::beginCycle() {
    if (!d->running)
        return;
    d->clearCycle();
    ++d->sequence;
    d->cycleTime.start();
    auto &record = d->current.record;
    record.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.workpieceId = QStringLiteral("SIM-%1").arg(d->sequence, 12, 10, QLatin1Char('0'));
    record.timestamp = QDateTime::currentDateTimeUtc();
    record.recipe = d->project.recipe;
    d->schedule(this, 0, [this] { executeNext(); });
}

void PipelineWorker::executeNext() {
    if (!d->running)
        return;
    while (d->index < d->project.flow.size() && !d->project.flow.at(d->index).enabled)
        ++d->index;
    if (d->index >= d->project.flow.size()) {
        QString error;
        if (!d->simulationHealthy(&error))
            fault(QStringLiteral("Virtual hardware fault: ") + error);
        else
            finishCycle();
        return;
    }
    const auto node = d->project.flow.at(d->index);
    const quint64 token = d->generation;
    if (d->triggerIndex != d->index && d->motionIndex != d->index && d->robotIndex != d->index)
        emit nodeActivated(node.id, node.title);
    if (!d->running || token != d->generation)
        return;
    QString error;
    if (!d->simulationHealthy(&error)) {
        fault(QStringLiteral("Virtual hardware fault: ") + error);
        return;
    }
    if (node.type == QLatin1String("trigger")) {
        int timeout = 0, poll = 0;
        if (!timingParameter(node, QStringLiteral("timeoutMs"), 1000, 60000, &timeout, &error) ||
            !timingParameter(node, QStringLiteral("pollMs"), 50, 1000, &poll, &error)) {
            fault(error);
            return;
        }
        if (d->triggerIndex != d->index) {
            d->triggerIndex = d->index;
            d->triggerTime.start();
        }
        const auto state = d->gateway->triggerReady(node, &error);
        if (state == TriggerReadiness::Failed) {
            fault(QStringLiteral("Trigger read failed: ") + error);
            return;
        }
        if (state == TriggerReadiness::Waiting) {
            const qint64 remaining = timeout - d->triggerTime.elapsed();
            if (remaining <= 0) {
                fault(QStringLiteral("Trigger '%1' timed out after %2 ms").arg(node.title).arg(timeout));
                return;
            }
            d->schedule(this, int(qMin<qint64>(poll, remaining)), [this] { executeNext(); });
            return;
        }
        d->triggerIndex = -1;
    } else if (node.type == QLatin1String("delay")) {
        ++d->index;
        d->schedule(this, node.parameters.value(QStringLiteral("ms")).toInt(), [this] { executeNext(); });
        return;
    } else if (node.type == QLatin1String("capture")) {
        const auto deviceId = node.parameters.value(QStringLiteral("deviceId"));
        if (!deviceId.isUndefined() && !deviceId.isString()) {
            fault(QStringLiteral("Capture parameter 'deviceId' must be a string when present"));
            return;
        }
        d->current.frame =
            d->gateway->capture(d->project.resolution, d->sequence, deviceId.toString(), &error);
        if (d->current.frame.isNull()) {
            fault(QStringLiteral("Camera acquisition failed: ") + error);
            return;
        }
        d->captured = true;
    } else if (node.type == QLatin1String("inspect")) {
        ImageMeasurement measurement;
        if (!d->captured || !analyzeFrame(d->current.frame, d->project.pixelsPerMm, &measurement, &error)) {
            fault(QStringLiteral("Image analysis failed: ") + error);
            return;
        }
        d->current.objectBounds = measurement.bounds;
        d->current.diameterPixels = measurement.diameterPixels;
        d->current.record.diameterMm = measurement.diameterMm;
        d->current.record.offsetXmm = measurement.offsetXmm;
        d->current.record.offsetYmm = measurement.offsetYmm;
        d->inspected = true;
    } else if (node.type == QLatin1String("judge")) {
        if (!d->inspected) {
            fault(QStringLiteral("Judgment requires a completed measurement"));
            return;
        }
        const double diameter = d->current.record.diameterMm;
        const bool accepted = diameter >= d->project.minDiameterMm && diameter <= d->project.maxDiameterMm;
        d->current.record.outcome = accepted ? QStringLiteral("OK") : QStringLiteral("NG");
        d->current.record.message = QStringLiteral("Diameter %1 mm; allowed %2..%3 mm")
                                        .arg(diameter, 0, 'f', 3)
                                        .arg(d->project.minDiameterMm, 0, 'f', 3)
                                        .arg(d->project.maxDiameterMm, 0, 'f', 3);
        d->judged = true;
    } else if (node.type == QLatin1String("store")) {
        if (!d->captured || !d->inspected || !d->judged || d->stored) {
            fault(QStringLiteral("Store requires exactly one captured, measured and judged result"));
            return;
        }
        const QString policy = d->project.imagePolicy;
        const bool keepImage =
            policy == QLatin1String("all") ||
            (policy == QLatin1String("ng") && d->current.record.outcome != QLatin1String("OK")) ||
            (policy == QLatin1String("sample") && d->sequence % 10 == 0);
        if (keepImage) {
            if (!ensureImageDirectory(d->workspace, &d->managedImages, &error)) {
                fault(error);
                return;
            }
            const QString imageName =
                QStringLiteral("ivs-") + QUuid::createUuid().toString(QUuid::Id128) + QStringLiteral(".png");
            d->pendingImage = QDir(d->managedImages).filePath(imageName);
            QSaveFile image(d->pendingImage);
            if (!image.open(QIODevice::WriteOnly) || !d->current.frame.save(&image, "PNG") ||
                !image.commit()) {
                image.cancelWriting();
                fault(QStringLiteral("Cannot save inspection image: ") + image.errorString());
                return;
            }
            if (!pruneImages(d->managedImages, d->project.imageRetentionDays, d->project.imageQuotaMB,
                             d->pendingImage, &error)) {
                fault(error);
                return;
            }
            d->current.record.imagePath = QDir(d->workspace).relativeFilePath(d->pendingImage);
        }
        d->current.record.durationMs = double(d->cycleTime.elapsed());
        if (!d->repository->append(d->current.record, &error) || d->current.record.id <= 0) {
            fault(QStringLiteral("Inspection result could not be committed: ") + error);
            return;
        }
        d->pendingImage.clear();
        d->stored = true;
        emit resultReady(d->current);
        if (!d->running || token != d->generation)
            return;
        emit logMessage(QStringLiteral("info"), QStringLiteral("Saved record #%1: %2, %3 mm")
                                                    .arg(d->current.record.id)
                                                    .arg(d->current.record.outcome)
                                                    .arg(d->current.record.diameterMm, 0, 'f', 3));
    } else if (node.type == QLatin1String("output")) {
        if (!d->stored) {
            fault(QStringLiteral("Result output requires a committed record"));
            return;
        }
        auto command = node;
        if (!command.parameters.contains(QStringLiteral("value"))) {
            const auto outcome = d->current.record.outcome;
            const bool coil = command.parameters.value(QStringLiteral("function"))
                                  .toString()
                                  .trimmed()
                                  .compare(QLatin1String("coil"), Qt::CaseInsensitive) == 0;
            const int value = coil ? (outcome == QLatin1String("OK") ? 1 : 0)
                                   : (outcome == QLatin1String("OK")   ? 1
                                      : outcome == QLatin1String("NG") ? 2
                                                                       : 3);
            command.parameters.insert(QStringLiteral("value"), value);
        }
        if (!d->gateway->execute(command, &error)) {
            fault(QStringLiteral("Result output failed: ") + error);
            return;
        }
    } else if (node.type == QLatin1String("robot_move") || node.type == QLatin1String("robot_grip") ||
               node.type == QLatin1String("robot_sort")) {
        const bool sorting = node.type == QLatin1String("robot_sort");
        if (sorting && !d->stored) {
            fault(QStringLiteral("Robot sorting requires a committed inspection record"));
            return;
        }
        int timeout = 0;
        if (!timingParameter(node, QStringLiteral("timeoutMs"), sorting ? 60000 : 30000, 60000, &timeout,
                             &error)) {
            fault(error);
            return;
        }
        if (d->robotIndex != d->index) {
            const bool issued = sorting
                                    ? d->gateway->executeRobotSort(node, d->current.record.outcome, &error)
                                    : d->gateway->execute(node, &error);
            if (!issued) {
                fault(QStringLiteral("Robot command failed: ") + error);
                return;
            }
            d->robotIndex = d->index;
            d->robotTime.start();
        }
        const auto readiness = d->gateway->robotReady(node, &error);
        if (readiness == TriggerReadiness::Failed) {
            fault(QStringLiteral("Robot command failed: ") + error);
            return;
        }
        if (readiness == TriggerReadiness::Waiting) {
            const qint64 remaining = timeout - d->robotTime.elapsed();
            if (remaining <= 0) {
                fault(QStringLiteral("Robot node '%1' timed out after %2 ms").arg(node.title).arg(timeout));
                return;
            }
            d->schedule(this, int(qMin<qint64>(20, remaining)), [this] { executeNext(); });
            return;
        }
        d->robotIndex = -1;
    } else if (node.type == QLatin1String("move")) {
        int timeout = 0;
        if (!timingParameter(node, QStringLiteral("timeoutMs"), 10000, 60000, &timeout, &error)) {
            fault(error);
            return;
        }
        if (d->motionIndex != d->index) {
            if (!d->gateway->execute(node, &error)) {
                fault(QStringLiteral("Motion command failed: ") + error);
                return;
            }
            d->motionIndex = d->index;
            d->motionTime.start();
        }
        const auto state = d->gateway->motionReady(node, &error);
        if (state == TriggerReadiness::Failed) {
            fault(QStringLiteral("Motion failed: ") + error);
            return;
        }
        if (state == TriggerReadiness::Waiting) {
            const qint64 remaining = timeout - d->motionTime.elapsed();
            if (remaining <= 0) {
                fault(QStringLiteral("Motion '%1' timed out after %2 ms").arg(node.title).arg(timeout));
                return;
            }
            d->schedule(this, int(qMin<qint64>(20, remaining)), [this] { executeNext(); });
            return;
        }
        d->motionIndex = -1;
    } else if (node.type == QLatin1String("light")) {
        if (!d->gateway->execute(node, &error)) {
            fault(QStringLiteral("Device command failed: ") + error);
            return;
        }
    } else {
        fault(QStringLiteral("Unknown flow node: ") + node.type);
        return;
    }
    if (!d->running || token != d->generation)
        return;
    ++d->index;
    d->schedule(this, 0, [this] { executeNext(); });
}

void PipelineWorker::finishCycle() {
    if (!d->running)
        return;
    if (!d->stored) {
        fault(QStringLiteral("Cycle ended without a committed inspection record"));
        return;
    }
    const qint64 elapsed = d->cycleTime.elapsed();
    const bool singleShot = d->singleShot;
    d->clearCycle();
    emit cycleFinished();
    if (!d->running)
        return;
    if (singleShot) {
        stop();
        return;
    }
    d->schedule(this, int(qMax<qint64>(0, d->project.cycleMs - elapsed)), [this] { beginCycle(); });
}

void PipelineWorker::fault(const QString &message) {
    d->cancel();
    emit logMessage(QStringLiteral("error"), message);
    emit stateChanged(QStringLiteral("fault"));
}

} // namespace ivs
