#include "RecordRepository.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QThread>
#include <QTimeZone>
#include <QUuid>
#include <QVariant>
#include <cmath>

namespace ivs {
namespace {
constexpr int MaximumPageSize = 1000;
constexpr int SeedBatchSize = 1000;

void clearError(QString *error) {
    if (error)
        error->clear();
}
bool fail(QString *error, const QString &message) {
    if (error)
        *error = message;
    return false;
}
QString utcString(const QDateTime &date) {
    return date.toUTC().toString(Qt::ISODateWithMs);
}
QString nonNull(const QString &text) {
    return text.isNull() ? QStringLiteral("") : text;
}
QString resolvedPath(const QString &path) {
    const QFileInfo info(path);
    return info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
}
bool samePath(const QString &a, const QString &b) {
#ifdef Q_OS_WIN
    return resolvedPath(a).compare(resolvedPath(b), Qt::CaseInsensitive) == 0;
#else
    return resolvedPath(a) == resolvedPath(b);
#endif
}
QString odbcValue(QString value) {
    value.replace('}', QStringLiteral("}}"));
    return '{' + value + '}';
}
QString csvCell(QString value) {
    // Whitespace can hide a spreadsheet formula, so prefix the whole cell.
    const QString trimmed = value.trimmed();
    if (!trimmed.isEmpty() && QStringLiteral("=+-@").contains(trimmed.front()))
        value.prepend('\'');
    else if (!value.isEmpty() && (value.front() == '\t' || value.front() == '\r' || value.front() == '\n'))
        value.prepend('\'');
    if (value.contains(',') || value.contains('"') || value.contains('\r') || value.contains('\n')) {
        value.replace('"', QStringLiteral("\"\""));
        value = '"' + value + '"';
    }
    return value;
}

const QString Columns = QStringLiteral("id,run_id,workpiece_id,timestamp_utc,recipe,outcome,diameter_mm,"
                                       "offset_x_mm,offset_y_mm,duration_ms,image_path,message");
const QString InsertColumns = QStringLiteral("run_id,workpiece_id,timestamp_utc,recipe,outcome,diameter_mm,"
                                             "offset_x_mm,offset_y_mm,duration_ms,image_path,message");
const QString Placeholders = QStringLiteral("?,?,?,?,?,?,?,?,?,?,?");

void bindRecord(QSqlQuery &query, const DetectionRecord &record) {
    query.bindValue(0, record.runId);
    query.bindValue(1, record.workpieceId);
    query.bindValue(2, utcString(record.timestamp));
    query.bindValue(3, nonNull(record.recipe));
    query.bindValue(4, record.outcome);
    query.bindValue(5, record.diameterMm);
    query.bindValue(6, record.offsetXmm);
    query.bindValue(7, record.offsetYmm);
    query.bindValue(8, record.durationMs);
    query.bindValue(9, nonNull(record.imagePath));
    query.bindValue(10, nonNull(record.message));
}

DetectionRecord readRecord(QSqlQuery &query) {
    DetectionRecord record;
    record.id = query.value(0).toLongLong();
    record.runId = query.value(1).toString();
    record.workpieceId = query.value(2).toString();
    record.timestamp = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs).toUTC();
    record.recipe = query.value(4).toString();
    record.outcome = query.value(5).toString();
    record.diameterMm = query.value(6).toDouble();
    record.offsetXmm = query.value(7).toDouble();
    record.offsetYmm = query.value(8).toDouble();
    record.durationMs = query.value(9).toDouble();
    record.imagePath = query.value(10).toString();
    record.message = query.value(11).toString();
    return record;
}

bool sameContents(const DetectionRecord &a, const DetectionRecord &b) {
    return a.runId == b.runId && a.workpieceId == b.workpieceId &&
           utcString(a.timestamp) == utcString(b.timestamp) && a.recipe == b.recipe &&
           a.outcome == b.outcome && a.diameterMm == b.diameterMm && a.offsetXmm == b.offsetXmm &&
           a.offsetYmm == b.offsetYmm && a.durationMs == b.durationMs && a.imagePath == b.imagePath &&
           a.message == b.message;
}

void closeConnection(const QString &name) {
    if (!QSqlDatabase::contains(name))
        return;
    {
        auto database = QSqlDatabase::database(name, false);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
}
} // namespace

class RecordRepository::Private {
  public:
    QString connectionName;
    QString path;
    QString table;
    bool sqlServer = false;
    QPointer<QObject> context;
    QPointer<QThread> owner;

    void close() {
        const auto name = connectionName;
        if (context) {
            QObject *object = context.data();
            if (QThread::currentThread() == owner) {
                closeConnection(name);
                delete object;
            } else {
                // No database handle crosses threads. Cleanup runs in the opening thread.
                QMetaObject::invokeMethod(
                    object,
                    [object, name] {
                        closeConnection(name);
                        delete object;
                    },
                    Qt::QueuedConnection);
            }
        }
        connectionName.clear();
        context.clear();
        owner.clear();
        path.clear();
    }

    bool ready(QString *error) const {
        if (connectionName.isEmpty() || !context || !owner)
            return fail(error, QStringLiteral("Database is not open."));
        if (owner != QThread::currentThread())
            return fail(error, QStringLiteral("Database access must run in the thread that opened it."));
        if (!QSqlDatabase::contains(connectionName) ||
            !QSqlDatabase::database(connectionName, false).isOpen())
            return fail(error, QStringLiteral("Database connection is closed."));
        return true;
    }

    bool where(const RecordFilter &filter, QString *sql, QList<QVariant> *values, QString *error) const {
        if (filter.beforeId < 0)
            return fail(error, QStringLiteral("The page cursor must not be negative."));
        if (filter.from.isValid() && filter.to.isValid() && filter.from > filter.to)
            return fail(error, QStringLiteral("The start date must not follow the end date."));
        if (!filter.outcome.isEmpty() && filter.outcome != QLatin1String("OK") &&
            filter.outcome != QLatin1String("NG") && filter.outcome != QLatin1String("ERROR"))
            return fail(error, QStringLiteral("Outcome must be OK, NG, or ERROR."));
        QStringList conditions;
        if (!filter.workpiece.isEmpty()) {
            conditions << QStringLiteral("workpiece_id = ?");
            *values << filter.workpiece;
        }
        if (!filter.outcome.isEmpty()) {
            conditions << QStringLiteral("outcome = ?");
            *values << filter.outcome;
        }
        if (filter.from.isValid()) {
            conditions << QStringLiteral("timestamp_utc >= ?");
            *values << utcString(filter.from);
        }
        if (filter.to.isValid()) {
            conditions << QStringLiteral("timestamp_utc <= ?");
            *values << utcString(filter.to);
        }
        if (filter.beforeId > 0) {
            conditions << QStringLiteral("id < ?");
            *values << filter.beforeId;
        }
        *sql = conditions.isEmpty() ? QString()
                                    : QStringLiteral(" WHERE ") + conditions.join(QStringLiteral(" AND "));
        return true;
    }

    QString insertStatement(bool returnId) const {
        return QStringLiteral("INSERT INTO ") + table + '(' + InsertColumns + ')' +
               ((sqlServer && returnId) ? QStringLiteral(" OUTPUT INSERTED.id") : QString()) +
               QStringLiteral(" VALUES (") + Placeholders + ')';
    }
};

RecordRepository::RecordRepository() : d(std::make_unique<Private>()) {}
RecordRepository::~RecordRepository() {
    d->close();
}

bool RecordRepository::open(const DatabaseConfig &config, const QString &workspace, QString *error) {
    clearError(error);
    if (d->context && d->owner != QThread::currentThread())
        return fail(error, QStringLiteral("Reopen the repository in its owning thread."));
    d->close();
    if (config.driver != QLatin1String("QSQLITE") && config.driver != QLatin1String("QODBC"))
        return fail(error, QStringLiteral("Supported database drivers are QSQLITE and QODBC (SQL Server)."));
    if (config.database.trimmed().isEmpty())
        return fail(error, QStringLiteral("Database name/path must not be empty."));
    if (!QSqlDatabase::isDriverAvailable(config.driver))
        return fail(error, QStringLiteral("Qt database driver is unavailable: %1").arg(config.driver));
    d->sqlServer = config.driver == QLatin1String("QODBC");
    if (d->sqlServer && (config.host.trimmed().isEmpty() || config.port < 1 || config.port > 65535 ||
                         config.odbcDriver.trimmed().isEmpty()))
        return fail(error, QStringLiteral("SQL Server requires a host, a valid port and an ODBC driver."));
    QString password;
    if (d->sqlServer && !config.user.isEmpty()) {
        if (config.passwordEnvironment.isEmpty() ||
            !qEnvironmentVariableIsSet(config.passwordEnvironment.toUtf8().constData()))
            return fail(error, QStringLiteral("SQL Server password environment variable is not set."));
        password = qEnvironmentVariable(config.passwordEnvironment.toUtf8().constData());
    }
    if (d->sqlServer) {
        d->path = config.host + ':' + QString::number(config.port) + '/' + config.database;
        d->table = QStringLiteral("dbo.inspection_records");
    } else {
        d->path = config.database == QLatin1String(":memory:")
                      ? config.database
                      : QFileInfo(QDir(workspace).absoluteFilePath(config.database)).absoluteFilePath();
        if (d->path != QLatin1String(":memory:") && !QDir().mkpath(QFileInfo(d->path).absolutePath()))
            return fail(error, QStringLiteral("Cannot create the database directory."));
        d->table = QStringLiteral("inspection_records");
    }
    d->connectionName = QStringLiteral("ivs-records-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    d->owner = QThread::currentThread();
    d->context = new QObject;
    const auto connectionName = d->connectionName;
    QObject *context = d->context.data();
    QObject::connect(
        d->owner, &QThread::finished, context,
        [connectionName, context] {
            closeConnection(connectionName);
            delete context;
        },
        Qt::DirectConnection);
    QString failure;
    {
        auto database = QSqlDatabase::addDatabase(config.driver, d->connectionName);
        if (d->sqlServer) {
            QString connection =
                QStringLiteral("Driver=%1;Server=%2;Database=%3;Encrypt=yes;TrustServerCertificate=%4;")
                    .arg(odbcValue(config.odbcDriver),
                         odbcValue(config.host + ',' + QString::number(config.port)),
                         odbcValue(config.database),
                         config.trustServerCertificate ? QStringLiteral("yes") : QStringLiteral("no"));
            if (config.user.isEmpty())
                connection += QStringLiteral("Trusted_Connection=yes;");
            else
                connection +=
                    QStringLiteral("UID=%1;PWD=%2;").arg(odbcValue(config.user), odbcValue(password));
            database.setDatabaseName(connection);
            database.setConnectOptions(QStringLiteral("SQL_ATTR_LOGIN_TIMEOUT=10"));
        } else {
            database.setDatabaseName(d->path);
            database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
        }
        if (!database.open()) {
            // ODBC diagnostics can echo connection strings; redact credentials before returning.
            failure = database.lastError().text();
            if (!password.isEmpty())
                failure.replace(password, QStringLiteral("[redacted]"));
        } else {
            QSqlQuery query(database);
            QStringList statements;
            if (d->sqlServer) {
                statements << QStringLiteral(
                    "IF OBJECT_ID(N'dbo.inspection_records', N'U') IS NULL BEGIN "
                    "CREATE TABLE dbo.inspection_records ("
                    "id BIGINT IDENTITY(1,1) NOT NULL PRIMARY KEY,"
                    "run_id NVARCHAR(128) NOT NULL UNIQUE,workpiece_id NVARCHAR(256) NOT NULL,"
                    "timestamp_utc VARCHAR(24) NOT NULL,recipe NVARCHAR(256) NOT NULL,"
                    "outcome VARCHAR(16) NOT NULL,diameter_mm FLOAT NOT NULL,offset_x_mm FLOAT NOT NULL,"
                    "offset_y_mm FLOAT NOT NULL,duration_ms FLOAT NOT NULL,"
                    "image_path NVARCHAR(2048) NOT NULL,message NVARCHAR(MAX) NOT NULL) END");
                for (const QString &suffix :
                     {QStringLiteral("workpiece"), QStringLiteral("outcome"), QStringLiteral("date")}) {
                    const QString column =
                        suffix == QLatin1String("workpiece") ? QStringLiteral("workpiece_id")
                        : suffix == QLatin1String("outcome") ? QStringLiteral("outcome")
                                                             : QStringLiteral("timestamp_utc");
                    statements
                        << QStringLiteral(
                               "IF NOT EXISTS (SELECT 1 FROM sys.indexes WHERE "
                               "object_id=OBJECT_ID(N'dbo.inspection_records') AND name=N'ix_records_%1') "
                               "CREATE INDEX ix_records_%1 ON dbo.inspection_records (%2,id DESC)")
                               .arg(suffix, column);
                }
            } else {
                statements
                    << QStringLiteral("PRAGMA journal_mode=WAL")
                    << QStringLiteral("PRAGMA synchronous=NORMAL")
                    << QStringLiteral(
                           "CREATE TABLE IF NOT EXISTS inspection_records ("
                           "id INTEGER PRIMARY KEY AUTOINCREMENT,run_id TEXT NOT NULL UNIQUE,workpiece_id "
                           "TEXT NOT NULL,"
                           "timestamp_utc TEXT NOT NULL,recipe TEXT NOT NULL,outcome TEXT NOT NULL,"
                           "diameter_mm REAL NOT NULL,offset_x_mm REAL NOT NULL,offset_y_mm REAL NOT NULL,"
                           "duration_ms REAL NOT NULL,image_path TEXT NOT NULL,message TEXT NOT NULL)")
                    << QStringLiteral("CREATE INDEX IF NOT EXISTS ix_records_workpiece ON "
                                      "inspection_records(workpiece_id,id DESC)")
                    << QStringLiteral("CREATE INDEX IF NOT EXISTS ix_records_outcome ON "
                                      "inspection_records(outcome,id DESC)")
                    << QStringLiteral("CREATE INDEX IF NOT EXISTS ix_records_date ON "
                                      "inspection_records(timestamp_utc,id DESC)");
            }
            // Probe columns to reject an unrelated/incompatible existing table.
            statements << QStringLiteral("SELECT ") + Columns + QStringLiteral(" FROM ") + d->table +
                              QStringLiteral(" WHERE 1=0");
            for (const auto &statement : statements) {
                if (!query.exec(statement)) {
                    failure = query.lastError().text();
                    break;
                }
                query.finish();
            }
        }
    }
    password.fill(QChar());
    if (!failure.isEmpty()) {
        d->close();
        return fail(error, QStringLiteral("Cannot open inspection database: ") + failure);
    }
    return true;
}

bool RecordRepository::append(DetectionRecord &record, QString *error) {
    clearError(error);
    if (!d->ready(error))
        return false;
    if (record.runId.trimmed().isEmpty() || record.workpieceId.trimmed().isEmpty() ||
        record.runId.size() > 128 || record.workpieceId.size() > 256 || record.recipe.size() > 256 ||
        record.imagePath.size() > 2048 || !record.timestamp.isValid() ||
        utcString(record.timestamp).size() != 24 ||
        (record.outcome != QLatin1String("OK") && record.outcome != QLatin1String("NG") &&
         record.outcome != QLatin1String("ERROR")) ||
        !std::isfinite(record.diameterMm) || !std::isfinite(record.offsetXmm) ||
        !std::isfinite(record.offsetYmm) || !std::isfinite(record.durationMs) || record.durationMs < 0)
        return fail(
            error,
            QStringLiteral(
                "Inspection record has invalid identifiers, timestamp, outcome, lengths or measurements."));
    const auto database = QSqlDatabase::database(d->connectionName, false);
    auto existing = [&](QString *lookupError) -> int {
        QSqlQuery lookup(database);
        if (!lookup.prepare(QStringLiteral("SELECT ") + Columns + QStringLiteral(" FROM ") + d->table +
                            QStringLiteral(" WHERE run_id=?"))) {
            fail(lookupError, lookup.lastError().text());
            return -1;
        }
        lookup.addBindValue(record.runId);
        if (!lookup.exec()) {
            fail(lookupError, lookup.lastError().text());
            return -1;
        }
        if (!lookup.next())
            return 0;
        const auto stored = readRecord(lookup);
        if (!sameContents(stored, record)) {
            fail(lookupError, QStringLiteral("A different inspection already uses this runId."));
            return -1;
        }
        record.id = stored.id;
        record.timestamp = stored.timestamp;
        return 1;
    };
    const int duplicate = existing(error);
    if (duplicate != 0)
        return duplicate == 1;
    QSqlQuery insert(database);
    if (!insert.prepare(d->insertStatement(true)))
        return fail(error, insert.lastError().text());
    bindRecord(insert, record);
    if (!insert.exec()) {
        const auto insertError = insert.lastError().text();
        // Another connection could have inserted this run since the lookup.
        const int raced = existing(error);
        if (raced != 0)
            return raced == 1;
        return fail(error, insertError);
    }
    if (d->sqlServer) {
        if (!insert.next())
            return fail(error, QStringLiteral("SQL Server did not return the inserted record id."));
        record.id = insert.value(0).toLongLong();
    } else
        record.id = insert.lastInsertId().toLongLong();
    record.timestamp = record.timestamp.toUTC();
    return true;
}

QList<DetectionRecord> RecordRepository::query(const RecordFilter &filter, QString *error) const {
    clearError(error);
    QList<DetectionRecord> records;
    if (!d->ready(error))
        return records;
    if (filter.limit <= 0) {
        fail(error, QStringLiteral("Page size must be positive."));
        return records;
    }
    QString where;
    QList<QVariant> values;
    if (!d->where(filter, &where, &values, error))
        return records;
    const int limit = qMin(filter.limit, MaximumPageSize);
    QString sql = QStringLiteral("SELECT ");
    if (d->sqlServer)
        sql += QStringLiteral("TOP (%1) ").arg(limit);
    sql += Columns + QStringLiteral(" FROM ") + d->table + where + QStringLiteral(" ORDER BY id DESC");
    if (!d->sqlServer)
        sql += QStringLiteral(" LIMIT %1").arg(limit);
    QSqlQuery query(QSqlDatabase::database(d->connectionName, false));
    query.setForwardOnly(true);
    if (!query.prepare(sql)) {
        fail(error, query.lastError().text());
        return records;
    }
    for (const auto &value : values)
        query.addBindValue(value);
    if (!query.exec()) {
        fail(error, query.lastError().text());
        return records;
    }
    records.reserve(limit);
    while (query.next())
        records.append(readRecord(query));
    if (query.lastError().isValid()) {
        fail(error, query.lastError().text());
        return {};
    }
    return records;
}

qint64 RecordRepository::count(const RecordFilter &filter, QString *error) const {
    clearError(error);
    if (!d->ready(error))
        return -1;
    QString where;
    QList<QVariant> values;
    if (!d->where(filter, &where, &values, error))
        return -1;
    QSqlQuery query(QSqlDatabase::database(d->connectionName, false));
    const QString aggregate = d->sqlServer ? QStringLiteral("COUNT_BIG(*)") : QStringLiteral("COUNT(*)");
    if (!query.prepare(QStringLiteral("SELECT ") + aggregate + QStringLiteral(" FROM ") + d->table + where)) {
        fail(error, query.lastError().text());
        return -1;
    }
    for (const auto &value : values)
        query.addBindValue(value);
    if (!query.exec() || !query.next()) {
        fail(error, query.lastError().text());
        return -1;
    }
    return query.value(0).toLongLong();
}

RecordSummary RecordRepository::summary(QString *error) const {
    clearError(error);
    RecordSummary result;
    if (!d->ready(error))
        return result;
    QSqlQuery query(QSqlDatabase::database(d->connectionName, false));
    const QString aggregate = d->sqlServer ? QStringLiteral("COUNT_BIG(*)") : QStringLiteral("COUNT(*)");
    if (!query.exec(QStringLiteral("SELECT outcome,") + aggregate + QStringLiteral(" FROM ") + d->table +
                    QStringLiteral(" GROUP BY outcome"))) {
        fail(error, query.lastError().text());
        return result;
    }
    while (query.next()) {
        const qint64 rows = query.value(1).toLongLong();
        const auto outcome = query.value(0).toString();
        result.total += rows;
        if (outcome == QLatin1String("OK"))
            result.ok += rows;
        else if (outcome == QLatin1String("NG"))
            result.ng += rows;
        else if (outcome == QLatin1String("ERROR"))
            result.error += rows;
    }
    if (query.lastError().isValid()) {
        fail(error, query.lastError().text());
        return {};
    }
    return result;
}

bool RecordRepository::exportCsv(const QString &path, const RecordFilter &filter, QString *error) const {
    clearError(error);
    if (!d->ready(error))
        return false;
    if (!d->sqlServer) {
        for (const QString &suffix :
             {QString(), QStringLiteral("-wal"), QStringLiteral("-shm"), QStringLiteral("-journal")}) {
            if (samePath(path, d->path + suffix))
                return fail(error, QStringLiteral(
                                       "CSV export must not overwrite the database or its journal files."));
        }
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return fail(error, file.errorString());
    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << "id,runId,workpieceId,timestampUtc,recipe,outcome,diameterMm,offsetXmm,offsetYmm,durationMs,"
              "imagePath,message\r\n";
    RecordFilter pageFilter = filter;
    pageFilter.limit = MaximumPageSize;
    while (true) {
        QString queryError;
        const auto page = query(pageFilter, &queryError);
        if (!queryError.isEmpty())
            return fail(error, queryError);
        for (const auto &record : page) {
            const QStringList cells{QString::number(record.id),
                                    record.runId,
                                    record.workpieceId,
                                    utcString(record.timestamp),
                                    record.recipe,
                                    record.outcome,
                                    QString::number(record.diameterMm, 'g', 17),
                                    QString::number(record.offsetXmm, 'g', 17),
                                    QString::number(record.offsetYmm, 'g', 17),
                                    QString::number(record.durationMs, 'g', 17),
                                    record.imagePath,
                                    record.message};
            for (int i = 0; i < cells.size(); ++i) {
                if (i)
                    stream << ',';
                // Numeric fields are authored here; keep negative numbers numeric.
                stream << ((i == 0 || (i >= 6 && i <= 9)) ? cells.at(i) : csvCell(cells.at(i)));
            }
            stream << "\r\n";
        }
        if (page.size() < MaximumPageSize)
            break;
        pageFilter.beforeId = page.last().id;
    }
    stream.flush();
    if (stream.status() != QTextStream::Ok)
        return fail(error, QStringLiteral("Failed while writing CSV."));
    if (!file.commit())
        return fail(error, file.errorString());
    return true;
}

bool RecordRepository::seed(qint64 count, QString *error, std::function<void(qint64)> progress,
                            const std::atomic_bool *cancel) {
    clearError(error);
    if (!d->ready(error))
        return false;
    if (count < 0 || count > 1000000000LL)
        return fail(error, QStringLiteral("Seed count must be between 0 and 1,000,000,000."));
    auto database = QSqlDatabase::database(d->connectionName, false);
    QSqlQuery maximum(database);
    if (!maximum.exec(QStringLiteral("SELECT COALESCE(MAX(id),0) FROM ") + d->table) || !maximum.next())
        return fail(error, maximum.lastError().text());
    const qint64 startingId = maximum.value(0).toLongLong();
    maximum.finish();
    const QString runPrefix =
        QStringLiteral("seed-") + QUuid::createUuid().toString(QUuid::WithoutBraces) + '-';
    const QDateTime epoch(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::UTC);
    QSqlQuery insert(database);
    if (!insert.prepare(d->insertStatement(false)))
        return fail(error, insert.lastError().text());
    qint64 completed = 0;
    while (completed < count) {
        if (cancel && cancel->load())
            return fail(error, QStringLiteral("Seed operation cancelled."));
        if (!database.transaction())
            return fail(error, database.lastError().text());
        const qint64 batchEnd = qMin(count, completed + SeedBatchSize);
        for (qint64 index = completed; index < batchEnd; ++index) {
            if (cancel && cancel->load()) {
                database.rollback();
                return fail(error,
                            QStringLiteral("Seed operation cancelled; current batch was rolled back."));
            }
            const qint64 serial = startingId + index + 1;
            DetectionRecord record;
            record.runId = runPrefix + QString::number(index);
            record.workpieceId = QStringLiteral("SIM-%1").arg(serial, 12, 10, QChar('0'));
            record.timestamp = epoch.addSecs(serial);
            record.recipe = QStringLiteral("Synthetic benchmark");
            record.outcome = serial % 20 == 0   ? QStringLiteral("ERROR")
                             : serial % 10 == 0 ? QStringLiteral("NG")
                                                : QStringLiteral("OK");
            record.diameterMm = 20.0 + double(serial % 201 - 100) / 100.0;
            record.offsetXmm = double(serial % 41 - 20) / 100.0;
            record.offsetYmm = double(serial % 61 - 30) / 100.0;
            record.durationMs = 8.0 + double(serial % 100) / 10.0;
            bindRecord(insert, record);
            if (!insert.exec()) {
                const auto sqlError = insert.lastError().text();
                database.rollback();
                return fail(error, sqlError);
            }
            insert.finish();
        }
        if (!database.commit()) {
            const auto sqlError = database.lastError().text();
            database.rollback();
            return fail(error, sqlError);
        }
        completed = batchEnd;
        if (progress)
            progress(completed);
    }
    return true;
}

QString RecordRepository::databasePath() const {
    return d->path;
}
} // namespace ivs
