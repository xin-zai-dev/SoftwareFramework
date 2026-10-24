#include "infrastructure/RecordRepository.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextStream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("ivs_data_tool"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Seed and benchmark a separate SQLite inspection database."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("database"), QStringLiteral("SQLite file to create/open (required)."),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("seed"), QStringLiteral("Synthetic rows to append, 0..1000000000."),
                      QStringLiteral("count"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("report"),
                      QStringLiteral("Also save the JSON report atomically to this path."),
                      QStringLiteral("json")});
    parser.process(app);

    const QString databasePath = parser.value(QStringLiteral("database"));
    const QString reportPath = parser.value(QStringLiteral("report"));
    bool reportPathValidated = false;
    bool parsedCount = false;
    const qint64 requested = parser.value(QStringLiteral("seed")).toLongLong(&parsedCount);
    QJsonObject report{{QStringLiteral("success"), false},
                       {QStringLiteral("driver"), QStringLiteral("QSQLITE")},
                       {QStringLiteral("database"), QFileInfo(databasePath).absoluteFilePath()},
                       {QStringLiteral("requestedSeedCount"), requested}};
    auto finish = [&](int exitCode, const QString &failure = QString()) {
        if (!failure.isEmpty())
            report.insert(QStringLiteral("error"), failure);
        QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (!reportPath.isEmpty() && reportPathValidated) {
            QString reportError;
            if (!QDir().mkpath(QFileInfo(reportPath).absolutePath())) {
                reportError = QStringLiteral("Cannot create benchmark report directory: %1")
                                  .arg(QFileInfo(reportPath).absolutePath());
            } else {
                QSaveFile output(reportPath);
                if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() ||
                    !output.commit())
                    reportError = output.errorString();
            }
            if (!reportError.isEmpty()) {
                QTextStream(stderr) << "Cannot save benchmark report: " << reportError << '\n';
                report.insert(QStringLiteral("success"), false);
                report.insert(QStringLiteral("reportError"), reportError);
                json = QJsonDocument(report).toJson(QJsonDocument::Indented);
                exitCode = 1;
            }
        }
        QTextStream(stdout) << json;
        if (!failure.isEmpty())
            QTextStream(stderr) << failure << '\n';
        return exitCode;
    };
    if (!parser.isSet(QStringLiteral("database")) || databasePath.trimmed().isEmpty())
        return finish(2, QStringLiteral("--database <path> is required; use a separate benchmark database."));
    if (databasePath == QLatin1String(":memory:"))
        return finish(2, QStringLiteral("Use a file database so that the benchmark is persistent."));
    if (!reportPath.isEmpty()) {
        auto resolved = [](const QString &path) {
            const QFileInfo info(path);
            return info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
        };
        for (const QString &suffix :
             {QString(), QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral("-journal")}) {
#ifdef Q_OS_WIN
            const bool conflicts =
                resolved(reportPath).compare(resolved(databasePath + suffix), Qt::CaseInsensitive) == 0;
#else
            const bool conflicts = resolved(reportPath) == resolved(databasePath + suffix);
#endif
            if (conflicts) {
                QTextStream(stderr) << "Report must not overwrite the database or its journal files.\n";
                return 2;
            }
        }
    }
    reportPathValidated = true;
    if (!parsedCount || requested < 0 || requested > 1000000000LL)
        return finish(2, QStringLiteral("--seed must be an integer between 0 and 1000000000."));

    ivs::DatabaseConfig config;
    config.database = databasePath;
    ivs::RecordRepository repository;
    QString error;
    if (!repository.open(config, QDir::currentPath(), &error))
        return finish(1, error);
    report.insert(QStringLiteral("database"), repository.databasePath());
    const qint64 before = repository.count({}, &error);
    if (before < 0)
        return finish(1, error);
    report.insert(QStringLiteral("rowsBefore"), before);

    QElapsedTimer elapsed;
    elapsed.start();
    const bool seeded = repository.seed(requested, &error);
    const double seedMs = double(elapsed.nsecsElapsed()) / 1000000.0;
    report.insert(QStringLiteral("seedElapsedMs"), seedMs);
    if (!seeded)
        return finish(1, error);
    elapsed.restart();
    const qint64 after = repository.count({}, &error);
    report.insert(QStringLiteral("countElapsedMs"), double(elapsed.nsecsElapsed()) / 1000000.0);
    if (after < 0)
        return finish(1, error);
    report.insert(QStringLiteral("rowsAfter"), after);
    report.insert(QStringLiteral("seededRows"), after - before);
    report.insert(QStringLiteral("seedRowsPerSecond"),
                  seedMs > 0 ? double(requested) * 1000.0 / seedMs : 0.0);
    if (after - before != requested)
        return finish(1, QStringLiteral("Actual row count does not match the requested append count."));

    ivs::RecordFilter filter;
    filter.limit = 100;
    QJsonArray pages;
    double totalMs = 0;
    double maximumMs = 0;
    qint64 sampledRows = 0;
    for (int index = 0; index < 20; ++index) {
        elapsed.restart();
        const auto rows = repository.query(filter, &error);
        const double pageMs = double(elapsed.nsecsElapsed()) / 1000000.0;
        if (!error.isEmpty())
            return finish(1, error);
        pages.append(QJsonObject{{QStringLiteral("page"), index + 1},
                                 {QStringLiteral("beforeId"), filter.beforeId},
                                 {QStringLiteral("rows"), rows.size()},
                                 {QStringLiteral("elapsedMs"), pageMs}});
        sampledRows += rows.size();
        totalMs += pageMs;
        maximumMs = qMax(maximumMs, pageMs);
        if (rows.size() < filter.limit)
            break;
        filter.beforeId = rows.last().id;
    }
    report.insert(QStringLiteral("queryPages"), pages);
    report.insert(QStringLiteral("sampledRows"), sampledRows);
    report.insert(QStringLiteral("averageQueryPageMs"), pages.isEmpty() ? 0 : totalMs / pages.size());
    report.insert(QStringLiteral("maximumQueryPageMs"), maximumMs);

    filter.beforeId = after / 2;
    elapsed.restart();
    const auto deepPage = repository.query(filter, &error);
    const double deepMs = double(elapsed.nsecsElapsed()) / 1000000.0;
    if (!error.isEmpty())
        return finish(1, error);
    report.insert(QStringLiteral("middleCursorQuery"),
                  QJsonObject{{QStringLiteral("beforeId"), filter.beforeId},
                              {QStringLiteral("rows"), deepPage.size()},
                              {QStringLiteral("elapsedMs"), deepMs}});
    const auto summary = repository.summary(&error);
    if (!error.isEmpty())
        return finish(1, error);
    report.insert(QStringLiteral("summary"), QJsonObject{{QStringLiteral("total"), summary.total},
                                                         {QStringLiteral("ok"), summary.ok},
                                                         {QStringLiteral("ng"), summary.ng},
                                                         {QStringLiteral("error"), summary.error}});
    if (summary.total != after)
        return finish(1, QStringLiteral("Summary total differs from the actual row count."));
    report.insert(QStringLiteral("success"), true);
    return finish(0);
}
