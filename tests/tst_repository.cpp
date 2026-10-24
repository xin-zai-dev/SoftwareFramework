#include "infrastructure/RecordRepository.h"

#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

using namespace ivs;

class RepositoryTest : public QObject {
    Q_OBJECT
  private:
    static DetectionRecord record(const QString &run, const QString &workpiece, const QString &outcome,
                                  const QString &time) {
        DetectionRecord r;
        r.runId = run;
        r.workpieceId = workpiece;
        r.outcome = outcome;
        r.timestamp = QDateTime::fromString(time, Qt::ISODateWithMs);
        r.recipe = QStringLiteral("Recipe 1");
        r.diameterMm = 20.25;
        r.offsetXmm = -0.3;
        r.offsetYmm = 0.4;
        r.durationMs = 18.5;
        return r;
    }
    static DatabaseConfig config() {
        DatabaseConfig c;
        c.database = QStringLiteral("data/test.sqlite");
        return c;
    }
  private slots:
    // Removing INSERT or UTC normalization loses data or changes the instant.
    void persistsRecordsAndNormalizesUtc() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        qint64 id = 0;
        {
            RecordRepository repo;
            QString error;
            QVERIFY2(repo.open(config(), directory.path(), &error), qPrintable(error));
            auto r = record("run-a", "piece-a", "OK", "2026-10-22T12:30:00.125+08:00");
            QVERIFY2(repo.append(r, &error), qPrintable(error));
            QVERIFY(r.id > 0);
            id = r.id;
        }
        RecordRepository reopened;
        QString error;
        QVERIFY2(reopened.open(config(), directory.path(), &error), qPrintable(error));
        const auto rows = reopened.query({}, &error);
        QCOMPARE(error, QString());
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().id, id);
        QCOMPARE(rows.first().timestamp.toString(Qt::ISODateWithMs), "2026-10-22T04:30:00.125Z");
        QCOMPARE(rows.first().offsetXmm, -0.3);
    }

    // A missing predicate, inclusive cursor or ignored limit changes these rows.
    void filtersAndPagesWithoutDuplicates() {
        QTemporaryDir directory;
        RecordRepository repo;
        QString error;
        QVERIFY2(repo.open(config(), directory.path(), &error), qPrintable(error));
        auto a = record("a", "A_1", "OK", "2026-10-20T00:00:00.000Z");
        auto b = record("b", "A_1", "NG", "2026-10-21T00:00:00.000Z");
        auto c = record("c", "AB1", "OK", "2026-10-22T00:00:00.000Z");
        auto e = record("d", "A_1", "ERROR", "2026-10-23T00:00:00.000Z");
        for (auto *r : {&a, &b, &c, &e})
            QVERIFY2(repo.append(*r, &error), qPrintable(error));
        RecordFilter filter;
        filter.workpiece = "A_1";
        QCOMPARE(repo.count(filter), 3);
        filter.outcome = "NG";
        QCOMPARE(repo.query(filter).first().runId, "b");
        filter.outcome.clear();
        filter.from = QDateTime::fromString("2026-10-21T00:00:00Z", Qt::ISODate);
        filter.to = QDateTime::fromString("2026-10-22T23:59:59Z", Qt::ISODate);
        QCOMPARE(repo.count(filter), 1);
        filter = {};
        filter.limit = 2;
        auto page1 = repo.query(filter);
        QCOMPARE(page1.size(), 2);
        QCOMPARE(page1.first().runId, "d");
        filter.beforeId = page1.last().id;
        auto page2 = repo.query(filter);
        QCOMPARE(page2.size(), 2);
        QCOMPARE(page2.first().runId, "b");
        QVERIFY(page2.first().id < page1.last().id);
        const auto totals = repo.summary();
        QCOMPARE(totals.total, 4);
        QCOMPARE(totals.ok, 2);
        QCOMPARE(totals.ng, 1);
        QCOMPARE(totals.error, 1);
    }

    // Removing the unique-run guard would silently add a second inspection.
    void repeatedRunIsIdempotentAndConflictingRunFails() {
        QTemporaryDir directory;
        RecordRepository repo;
        QString error;
        QVERIFY2(repo.open(config(), directory.path(), &error), qPrintable(error));
        auto first = record("repeat", "P1", "OK", "2026-10-22T00:00:00.000Z");
        auto repeated = first;
        QVERIFY(repo.append(first, &error));
        QVERIFY2(repo.append(repeated, &error), qPrintable(error));
        QCOMPARE(first.id, repeated.id);
        QCOMPARE(repo.count(), 1);
        repeated.diameterMm = 99;
        QVERIFY(!repo.append(repeated, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(repo.count(), 1);
    }

    // Single-page export or unescaped formula/quote/newline corrupts this file.
    void csvStreamsAllPagesAndEscapesSpreadsheetCells() {
        QTemporaryDir directory;
        RecordRepository repo;
        QString error;
        QVERIFY2(repo.open(config(), directory.path(), &error), qPrintable(error));
        QVERIFY2(repo.seed(2050, &error), qPrintable(error));
        auto quoted = record("csv-run", "=SUM(1,2)", "NG", "2026-10-22T00:00:00.000Z");
        quoted.recipe = "a,\"b\"\nc";
        quoted.message = "  +cmd";
        QVERIFY(repo.append(quoted, &error));
        const auto path = directory.filePath("export.csv");
        QVERIFY2(repo.exportCsv(path, {}, &error), qPrintable(error));
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto csv = file.readAll();
        QVERIFY(csv.contains("\"'=SUM(1,2)\""));
        QVERIFY(csv.contains("\"a,\"\"b\"\"\nc\""));
        QVERIFY(csv.contains("'  +cmd"));
        QCOMPARE(csv.count("seed-"), 2050);
        QVERIFY(csv.contains("csv-run"));
        RecordFilter oversized;
        oversized.limit = 1000000;
        QCOMPARE(repo.query(oversized).size(), 1000);
    }

    // Exporting through a symlink must not replace the open database file.
    void csvDoesNotOverwriteDatabaseOrItsAliases() {
        QTemporaryDir directory;
        RecordRepository repo;
        QString error;
        QVERIFY(repo.open(config(), directory.path(), &error));
        QVERIFY(repo.seed(1, &error));
        QVERIFY(!repo.exportCsv(repo.databasePath(), {}, &error));
#ifdef Q_OS_UNIX
        const QString alias = directory.filePath("database-alias.sqlite");
        QVERIFY(QFile::link(repo.databasePath(), alias));
        QVERIFY(!repo.exportCsv(alias, {}, &error));
#endif
        QCOMPARE(repo.count(), 1);
    }

    // Invalid driver/file inputs must fail without pretending data is saved.
    void rejectsMalformedOpenAndInvalidWrites() {
        QTemporaryDir directory;
        QFile malformed(directory.filePath("broken.sqlite"));
        QVERIFY(malformed.open(QIODevice::WriteOnly));
        malformed.write("this is not a sqlite database");
        malformed.close();
        RecordRepository repo;
        auto bad = config();
        bad.database = malformed.fileName();
        QString error;
        QVERIFY(!repo.open(bad, directory.path(), &error));
        QVERIFY(!error.isEmpty());
        bad.driver = "not-a-driver";
        QVERIFY(!repo.open(bad, directory.path(), &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(repo.open(config(), directory.path(), &error));
        DetectionRecord invalid;
        QVERIFY(!repo.append(invalid, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(repo.count(), 0);
        RecordFilter reversed;
        reversed.from = QDateTime::currentDateTimeUtc();
        reversed.to = reversed.from.addDays(-1);
        QCOMPARE(repo.count(reversed, &error), -1);
        QVERIFY(!error.isEmpty());
    }

    // Cancellation must stop at a bounded batch and preserve committed records.
    void seedCancellationIsBounded() {
        QTemporaryDir directory;
        RecordRepository repo;
        QString error;
        QVERIFY(repo.open(config(), directory.path(), &error));
        std::atomic_bool cancel{false};
        QVERIFY(!repo.seed(
            2500, &error,
            [&cancel](qint64 done) {
                if (done >= 1000)
                    cancel.store(true);
            },
            &cancel));
        QCOMPARE(repo.count(), 1000);
        QVERIFY(!error.isEmpty());
        cancel.store(false);
        QVERIFY2(repo.seed(10, &error, {}, &cancel), qPrintable(error));
        QCOMPARE(repo.count(), 1010);
    }
};

QTEST_GUILESS_MAIN(RepositoryTest)
#include "tst_repository.moc"
