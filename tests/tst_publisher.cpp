#include "application/Publisher.h"
#include "domain/Project.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using namespace ivs;

class PublisherTest : public QObject {
    Q_OBJECT
  private slots:
    // Qt's default SQL-directory deployment pulls unrelated broken vendor drivers.
    void pluginSelectionIncludesOnlyRequiredSqlDrivers() {
        DatabaseConfig database;
        auto selected = requiredRuntimePlugins(database);
#ifdef Q_OS_MACOS
        QVERIFY(selected.contains("platforms/libqcocoa.dylib"));
        QVERIFY(selected.contains("sqldrivers/libqsqlite.dylib"));
        QVERIFY(!selected.contains("sqldrivers/libqsqlodbc.dylib"));
        QCOMPARE(selected.size(), 2);
        database.driver = "QODBC";
        selected = requiredRuntimePlugins(database);
        QVERIFY(selected.contains("sqldrivers/libqsqlite.dylib"));
        QVERIFY(selected.contains("sqldrivers/libqsqlodbc.dylib"));
        QCOMPARE(selected.size(), 3);
#elif defined(Q_OS_WIN)
        QVERIFY(selected.contains("platforms/qwindows.dll"));
        QVERIFY(selected.contains("sqldrivers/qsqlite.dll"));
        QCOMPARE(selected.size(), 2);
        database.driver = "QODBC";
        selected = requiredRuntimePlugins(database);
        QVERIFY(selected.contains("sqldrivers/qsqlite.dll"));
        QVERIFY(selected.contains("sqldrivers/qsqlodbc.dll"));
        QCOMPARE(selected.size(), 3);
#else
        QVERIFY(selected.isEmpty());
#endif
        for (const auto &path : selected) {
            QVERIFY(!path.contains("psql"));
            QVERIFY(!path.contains("mimer"));
        }
    }

    // Publishing must not silently replace SQL Server with a different backend.
    void runtimeProjectPreservesSqlServerSettings() {
        ProjectConfig source = defaultProject();
        source.database.driver = "QODBC";
        source.database.database = "InspectionProduction";
        source.database.host = "sql.internal";
        source.database.port = 1533;
        source.database.user = "operator";
        source.database.passwordEnvironment = "IVS_PUBLISH_TEST_ENV";
        source.database.odbcDriver = "ODBC Driver 18 for SQL Server";
        source.database.trustServerCertificate = true;
        const auto exported = projectToJson(runtimeProjectForPublishing(source));
        const auto database = exported.value("database").toObject();
        QCOMPARE(database.value("driver").toString(), "QODBC");
        QCOMPARE(database.value("database").toString(), "InspectionProduction");
        QCOMPARE(database.value("host").toString(), "sql.internal");
        QCOMPARE(database.value("port").toInt(), 1533);
        QCOMPARE(database.value("user").toString(), "operator");
        QCOMPARE(database.value("passwordEnvironment").toString(), "IVS_PUBLISH_TEST_ENV");
        QCOMPARE(database.value("odbcDriver").toString(), "ODBC Driver 18 for SQL Server");
        QVERIFY(database.value("trustServerCertificate").toBool());
        QVERIFY(!database.contains("password"));
        QVERIFY(validateProject(runtimeProjectForPublishing(source)).isEmpty());
    }

    // Local data paths must become relative, and credential parameters stay private.
    void runtimeProjectUsesFreshSqliteAndRemovesCredentials() {
        ProjectConfig source = defaultProject();
        source.database.database = "/production/private.sqlite";
        source.devices.first().parameters = {{"password", "camera-secret"},
                                             {"channel", 3},
                                             {"nested", QJsonObject{{"api_key", "hidden"}, {"keep", 4}}}};
        source.flow.first().parameters.insert("token", "node-secret");
        const auto exported = runtimeProjectForPublishing(source);
        QCOMPARE(exported.database.driver, "QSQLITE");
        QCOMPARE(exported.database.database, "data/inspection.sqlite");
        QVERIFY(!exported.devices.first().parameters.contains("password"));
        QCOMPARE(exported.devices.first().parameters.value("channel").toInt(), 3);
        const auto nested = exported.devices.first().parameters.value("nested").toObject();
        QVERIFY(!nested.contains("api_key"));
        QCOMPARE(nested.value("keep").toInt(), 4);
        QVERIFY(!exported.flow.first().parameters.contains("token"));
        QCOMPARE(source.database.database, "/production/private.sqlite");
        QVERIFY(source.devices.first().parameters.contains("password"));
    }

    // A missing source must not create a misleading runtime directory.
    void rejectsMissingExecutable() {
        QTemporaryDir directory;
        const QString destination = directory.filePath("runtime");
        const auto result =
            publishRuntime(defaultProject(), destination, directory.filePath("missing-binary"));
        QVERIFY(!result.success);
        QVERIFY(result.error.contains("executable", Qt::CaseInsensitive));
        QVERIFY(!QFileInfo::exists(destination));
    }

    // Reusing a target could overwrite an existing deployment or private data.
    void preservesExistingDestination() {
        QTemporaryDir directory;
        const QString destination = directory.filePath("existing");
        QVERIFY(QDir().mkpath(destination));
        QFile sentinel(QDir(destination).filePath("sentinel.txt"));
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("keep me"), 7);
        sentinel.close();
        const auto result =
            publishRuntime(defaultProject(), destination, QCoreApplication::applicationFilePath());
        QVERIFY(!result.success);
        QVERIFY(result.error.contains("exists", Qt::CaseInsensitive));
        QVERIFY(sentinel.open(QIODevice::ReadOnly));
        QCOMPARE(sentinel.readAll(), QByteArray("keep me"));
    }

    // A malformed project must fail before any files are published.
    void rejectsInvalidProject() {
        QTemporaryDir directory;
        ProjectConfig invalid = defaultProject();
        invalid.resolution = QSize(0, 0);
        const QString destination = directory.filePath("invalid-runtime");
        const auto result = publishRuntime(invalid, destination, QCoreApplication::applicationFilePath());
        QVERIFY(!result.success);
        QVERIFY(result.error.contains("project", Qt::CaseInsensitive));
        QVERIFY(!QFileInfo::exists(destination));
    }
};

QTEST_GUILESS_MAIN(PublisherTest)
#include "tst_publisher.moc"
