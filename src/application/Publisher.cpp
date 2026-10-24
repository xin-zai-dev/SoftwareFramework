#include "Publisher.h"
#include "domain/Project.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSet>
#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <unistd.h>
#endif

namespace ivs {
namespace {
QString resolveFuturePath(const QString &path) {
    const QFileInfo info(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    if (info.exists())
        return info.canonicalFilePath();
    if (info.absoluteFilePath() == info.absolutePath())
        return info.absoluteFilePath();
    return QDir(resolveFuturePath(info.absolutePath())).filePath(info.fileName());
}

bool within(const QString &path, const QString &root) {
#ifdef Q_OS_WIN
    constexpr auto sensitivity = Qt::CaseInsensitive;
#else
    constexpr auto sensitivity = Qt::CaseSensitive;
#endif
    return path.compare(root, sensitivity) == 0 || path.startsWith(root + '/', sensitivity);
}

bool writeFile(const QString &path, const QByteArray &bytes, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = QStringLiteral("Cannot write %1: %2").arg(QFileInfo(path).fileName(), file.errorString());
        return false;
    }
    return true;
}

QJsonValue withoutCredentials(const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray output;
        for (const auto &item : value.toArray())
            output.append(withoutCredentials(item));
        return output;
    }
    if (!value.isObject())
        return value;
    static const QSet<QString> secretKeys{
        QStringLiteral("password"),         QStringLiteral("passwd"),     QStringLiteral("pwd"),
        QStringLiteral("secret"),           QStringLiteral("token"),      QStringLiteral("accesstoken"),
        QStringLiteral("apikey"),           QStringLiteral("credential"), QStringLiteral("credentials"),
        QStringLiteral("connectionstring"), QStringLiteral("dsn")};
    QJsonObject output;
    const auto input = value.toObject();
    for (auto it = input.begin(); it != input.end(); ++it) {
        QString normalized = it.key().toLower();
        normalized.remove('_');
        normalized.remove('-');
        if (!secretKeys.contains(normalized))
            output.insert(it.key(), withoutCredentials(it.value()));
    }
    return output;
}

bool privateArtifact(const QString &relative) {
    const QString lower = relative.toLower();
    const QStringList parts = lower.split('/');
    for (const auto &part : parts) {
        if (part == QLatin1String(".git") || part == QLatin1String(".codex") ||
            part == QLatin1String(".env") || part.startsWith(QLatin1String(".env.")) ||
            part == QLatin1String("data") || part == QLatin1String("captures") ||
            part == QLatin1String("inspection-images") || part == QLatin1String("workspace") ||
            part == QLatin1String("logs"))
            return true;
    }
    if (lower.startsWith(QLatin1String("contents/macos/images/")) ||
        lower.startsWith(QLatin1String("contents/resources/images/")))
        return true;
    return lower.endsWith(QLatin1String(".sqlite")) || lower.endsWith(QLatin1String(".sqlite3")) ||
           lower.endsWith(QLatin1String(".db")) || lower.endsWith(QLatin1String(".ivproj")) ||
           lower.endsWith(QLatin1String("-wal")) || lower.endsWith(QLatin1String("-shm")) ||
           lower.endsWith(QLatin1String("-journal"));
}

bool commercialSdkFile(const QString &path) {
    const QString name = QFileInfo(path).fileName().toLower();
    return name.contains(QLatin1String("halcon")) || name.contains(QLatin1String("mvcameracontrol")) ||
           name.contains(QLatin1String("mvgigevisionsdk")) || name.contains(QLatin1String("mvusb3")) ||
           name.startsWith(QLatin1String("libgts.")) || name.startsWith(QLatin1String("ltdmc."));
}

bool copyTree(const QString &source, const QString &destination, const QString &sourceRoot,
              const QString &destinationRoot, QStringList *excluded, QString *error) {
    const QFileInfo info(source);
    const QString relative = QDir(sourceRoot).relativeFilePath(source);
    // Re-publishing a previously deployed bundle must not retain unselected plugins.
    if (relative.compare(QStringLiteral("Contents/PlugIns"), Qt::CaseInsensitive) == 0) {
        excluded->append(relative);
        return true;
    }
    if (relative != QLatin1String(".") && privateArtifact(relative)) {
        excluded->append(relative);
        return true;
    }
    if (commercialSdkFile(source)) {
        *error = QStringLiteral("Commercial SDK redistribution requires a separate deployment workflow: %1")
                     .arg(relative);
        return false;
    }
    if (info.isSymLink()) {
#ifdef Q_OS_UNIX
        const QByteArray nativeSource = QFile::encodeName(source);
        QByteArray rawTarget(4096, '\0');
        const auto length = ::readlink(nativeSource.constData(), rawTarget.data(), size_t(rawTarget.size()));
        if (length < 0 || length >= rawTarget.size()) {
            *error = QStringLiteral("Cannot read bundle symbolic link: %1").arg(relative);
            return false;
        }
        rawTarget.resize(qsizetype(length));
        const QString targetText = QFile::decodeName(rawTarget);
        const QString absoluteTarget =
            QFileInfo(QDir(info.absolutePath()).absoluteFilePath(targetText)).canonicalFilePath();
        if (absoluteTarget.isEmpty() || !within(absoluteTarget, sourceRoot)) {
            *error =
                QStringLiteral("Bundle symbolic link escapes the application or is broken: %1").arg(relative);
            return false;
        }
        // Keep ordinary framework links exactly; rewrite internal absolute links relatively.
        if (QDir::isAbsolutePath(targetText)) {
            const QString copiedTarget =
                QDir(destinationRoot).filePath(QDir(sourceRoot).relativeFilePath(absoluteTarget));
            rawTarget =
                QFile::encodeName(QDir(QFileInfo(destination).absolutePath()).relativeFilePath(copiedTarget));
        }
        if (::symlink(rawTarget.constData(), QFile::encodeName(destination).constData()) != 0) {
            *error = QStringLiteral("Cannot preserve bundle symbolic link %1: %2")
                         .arg(relative, QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }
        return true;
#else
        *error =
            QStringLiteral("Symbolic link copying is unavailable for this bundle on the current platform.");
        return false;
#endif
    }
    if (info.isDir()) {
        if (!QDir().mkpath(destination)) {
            *error = QStringLiteral("Cannot create bundle directory: %1").arg(relative);
            return false;
        }
        const auto entries = QDir(source).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const auto &entry : entries) {
            if (!copyTree(entry.absoluteFilePath(), QDir(destination).filePath(entry.fileName()), sourceRoot,
                          destinationRoot, excluded, error))
                return false;
        }
        return true;
    }
    if (!info.isFile() || !QFile::copy(source, destination) ||
        !QFile::setPermissions(destination, info.permissions())) {
        *error = QStringLiteral("Cannot copy application file: %1").arg(relative);
        return false;
    }
    return true;
}

struct ProcessResult {
    bool success = false;
    int exitCode = -1;
    QByteArray output;
    QString error;
};

ProcessResult runProcess(const QString &program, const QStringList &arguments,
                         const QString &workingDirectory, int timeoutMs = 180000) {
    ProcessResult result;
    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForStarted(10000)) {
        result.error = process.errorString();
        return result;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (!process.waitForFinished(1000)) {
        if (elapsed.elapsed() > timeoutMs) {
            process.kill();
            process.waitForFinished(3000);
            result.output = process.readAll();
            result.error = QStringLiteral("Deployment process timed out.");
            return result;
        }
        if (process.state() == QProcess::NotRunning)
            break;
    }
    result.exitCode = process.exitCode();
    result.output = process.readAll();
    result.success = process.exitStatus() == QProcess::NormalExit && result.exitCode == 0;
    if (!result.success)
        result.error = QStringLiteral("Process failed with exit code %1.").arg(result.exitCode);
    return result;
}

QString shellLiteral(QString text) {
    text.replace('\'', QStringLiteral("'\"'\"'"));
    return '\'' + text + '\'';
}
} // namespace

ProjectConfig runtimeProjectForPublishing(const ProjectConfig &project) {
    ProjectConfig runtimeProject = project;
    if (runtimeProject.database.driver == QLatin1String("QSQLITE"))
        runtimeProject.database.database = QStringLiteral("data/inspection.sqlite");
    for (auto &device : runtimeProject.devices)
        device.parameters = withoutCredentials(device.parameters).toObject();
    for (auto &node : runtimeProject.flow)
        node.parameters = withoutCredentials(node.parameters).toObject();
    return runtimeProject;
}

QStringList requiredRuntimePlugins(const DatabaseConfig &database) {
    QStringList plugins;
#ifdef Q_OS_MACOS
    plugins = {QStringLiteral("platforms/libqcocoa.dylib"), QStringLiteral("sqldrivers/libqsqlite.dylib")};
    if (database.driver == QLatin1String("QODBC"))
        plugins.append(QStringLiteral("sqldrivers/libqsqlodbc.dylib"));
#elif defined(Q_OS_WIN)
    plugins = {QStringLiteral("platforms/qwindows.dll"), QStringLiteral("sqldrivers/qsqlite.dll")};
    if (database.driver == QLatin1String("QODBC"))
        plugins.append(QStringLiteral("sqldrivers/qsqlodbc.dll"));
#else
    Q_UNUSED(database)
#endif
    return plugins;
}

PublishResult publishRuntime(const ProjectConfig &project, const QString &destination,
                             const QString &sourceExecutable, const QString &qtBinDirectory) {
    PublishResult result;
    const auto reject = [&](const QString &error) {
        result.error = error;
        return result;
    };
    if (destination.trimmed().isEmpty())
        return reject(QStringLiteral("Publish destination must not be empty."));
    const QFileInfo destinationInfo(QDir::cleanPath(QFileInfo(destination).absoluteFilePath()));
    if (destinationInfo.exists() || destinationInfo.isSymLink())
        return reject(QStringLiteral("Publish destination already exists; choose a new directory."));
    const auto validationErrors = validateProject(project);
    if (!validationErrors.isEmpty())
        return reject(QStringLiteral("Invalid project: ") + validationErrors.join('\n'));
    const QFileInfo executableInfo(sourceExecutable);
    if (!executableInfo.exists() || !executableInfo.isFile() || !executableInfo.isExecutable())
        return reject(QStringLiteral("Source executable is missing or is not executable."));
    result.directory = resolveFuturePath(destinationInfo.absoluteFilePath());

    QString deploymentToolName;
    QString sourceBundle;
    QString relativeExecutable;
    QString relativeBundle;
#ifdef Q_OS_MACOS
    QDir ancestor(executableInfo.absolutePath());
    while (!ancestor.isRoot()) {
        if (ancestor.dirName().endsWith(QLatin1String(".app"), Qt::CaseInsensitive)) {
            sourceBundle = ancestor.canonicalPath();
            break;
        }
        ancestor.cdUp();
    }
    if (sourceBundle.isEmpty())
        return reject(QStringLiteral("Source executable must be inside a macOS .app bundle."));
    if (!within(executableInfo.canonicalFilePath(), sourceBundle))
        return reject(QStringLiteral("Source executable escapes its application bundle."));
    if (within(result.directory, sourceBundle))
        return reject(QStringLiteral("Publish destination must be outside the source application bundle."));
    relativeBundle = QFileInfo(sourceBundle).fileName();
    relativeExecutable =
        relativeBundle + '/' + QDir(sourceBundle).relativeFilePath(executableInfo.canonicalFilePath());
    deploymentToolName = QStringLiteral("macdeployqt");
#elif defined(Q_OS_WIN)
    relativeExecutable = QStringLiteral("IndustrialVisionStudio.exe");
    deploymentToolName = QStringLiteral("windeployqt.exe");
#else
    Q_UNUSED(qtBinDirectory)
    return reject(QStringLiteral("Portable Linux runtime packaging is unavailable in this build; use a "
                                 "verified CMake install/package workflow."));
#endif

    const QString qtDirectory = qtBinDirectory.isEmpty() ? QLibraryInfo::path(QLibraryInfo::BinariesPath)
                                                         : QFileInfo(qtBinDirectory).absoluteFilePath();
    const QString deploymentTool = QDir(qtDirectory).filePath(deploymentToolName);
    if (!QFileInfo(deploymentTool).isExecutable())
        return reject(QStringLiteral("Matching Qt deployment tool is unavailable: %1").arg(deploymentTool));
#ifdef Q_OS_WIN
    const QString qmake = QDir(qtDirectory).filePath(QStringLiteral("qmake.exe"));
#else
    const QString qmake = QDir(qtDirectory).filePath(QStringLiteral("qmake"));
#endif
    if (!QFileInfo(qmake).isExecutable())
        return reject(
            QStringLiteral("Cannot verify the deployment tool's Qt version: sibling qmake is unavailable."));
    const auto versionCheck = runProcess(qmake, {QStringLiteral("-query"), QStringLiteral("QT_VERSION")},
                                         executableInfo.absolutePath(), 10000);
    const QString toolVersion = QString::fromUtf8(versionCheck.output).trimmed();
    if (!versionCheck.success || toolVersion != QString::fromLatin1(qVersion()))
        return reject(QStringLiteral("Deployment Qt version (%1) must match the running Qt version (%2).")
                          .arg(toolVersion, QString::fromLatin1(qVersion())));
    const auto pluginDirectoryCheck =
        runProcess(qmake, {QStringLiteral("-query"), QStringLiteral("QT_INSTALL_PLUGINS")},
                   executableInfo.absolutePath(), 10000);
    const QString sourcePluginRoot =
        QFileInfo(QString::fromUtf8(pluginDirectoryCheck.output).trimmed()).canonicalFilePath();
    if (!pluginDirectoryCheck.success || sourcePluginRoot.isEmpty() || !QFileInfo(sourcePluginRoot).isDir())
        return reject(QStringLiteral("Cannot locate plugins from the matching Qt installation."));
    QStringList selectedPlugins = requiredRuntimePlugins(project.database);
    for (const auto &plugin : selectedPlugins) {
        if (!QFileInfo::exists(QDir(sourcePluginRoot).filePath(plugin)))
            return reject(QStringLiteral("Required Qt plugin is unavailable: %1").arg(plugin));
    }
#ifdef Q_OS_MACOS
    const QStringList optionalPlugins{QStringLiteral("imageformats/libqjpeg.dylib"),
                                      QStringLiteral("platforms/libqoffscreen.dylib")};
#elif defined(Q_OS_WIN)
    const QStringList optionalPlugins{QStringLiteral("imageformats/qjpeg.dll"),
                                      QStringLiteral("platforms/qoffscreen.dll")};
    const auto toolHelp =
        runProcess(deploymentTool, {QStringLiteral("--help")}, executableInfo.absolutePath(), 10000);
    if (!toolHelp.output.contains("--no-plugins"))
        return reject(
            QStringLiteral("This windeployqt does not support controlled plugin deployment (--no-plugins)."));
#else
    const QStringList optionalPlugins;
#endif
    for (const auto &plugin : optionalPlugins) {
        if (QFileInfo::exists(QDir(sourcePluginRoot).filePath(plugin)))
            selectedPlugins.append(plugin);
    }

#ifdef Q_OS_MACOS
    // macdeployqt can copy non-Qt dylibs too. Reject optional third-party linkage
    // before invoking it; SDK redistribution and licenses are outside this publisher.
    const auto dependencies = runProcess(QStringLiteral("/usr/bin/otool"),
                                         {QStringLiteral("-L"), executableInfo.canonicalFilePath()},
                                         executableInfo.absolutePath(), 10000);
    if (!dependencies.success)
        return reject(QStringLiteral("Cannot inspect executable dependencies before deployment."));
    for (const auto &line : QString::fromUtf8(dependencies.output).split('\n')) {
        if (!line.startsWith('\t'))
            continue;
        const QString library = line.trimmed().section(QStringLiteral(" ("), 0, 0);
        const bool systemLibrary = library.startsWith(QLatin1String("/usr/lib/")) ||
                                   library.startsWith(QLatin1String("/System/Library/"));
        const bool qtLibrary =
            library.contains(QLatin1String("/Qt")) && library.contains(QLatin1String(".framework/"));
        if (commercialSdkFile(library) || (!systemLibrary && !qtLibrary))
            return reject(
                QStringLiteral("Optional non-Qt dependency needs a separate verified deployment workflow: %1")
                    .arg(library));
    }
#endif

    QDir parent(QFileInfo(result.directory).absolutePath());
    if ((!parent.exists() && !QDir().mkpath(parent.absolutePath())) ||
        !parent.mkdir(QFileInfo(result.directory).fileName()))
        return reject(QStringLiteral("Cannot create a new publish directory (it may already exist)."));

    const ProjectConfig runtimeProject = runtimeProjectForPublishing(project);
    QJsonObject manifest{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("success"), false},
        {QStringLiteral("createdUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("qtVersion"), QString::fromLatin1(qVersion())},
        {QStringLiteral("deploymentTool"), deploymentTool},
        {QStringLiteral("deploymentToolQtVersion"), toolVersion},
        {QStringLiteral("project"), QStringLiteral("project.ivproj")},
        {QStringLiteral("executable"), relativeExecutable},
        {QStringLiteral("databaseDriver"), runtimeProject.database.driver},
        {QStringLiteral("database"), runtimeProject.database.database},
        {QStringLiteral("productionDataCopied"), false},
        {QStringLiteral("commercialSdkRedistributablesIncluded"), false}};
    QByteArray log;
    QString error;
    auto saveDiagnostics = [&] {
        QString diagnosticError;
        const bool logSaved = writeFile(QDir(result.directory).filePath(QStringLiteral("deployment.log")),
                                        log, &diagnosticError);
        if (!logSaved) {
            manifest.insert(QStringLiteral("success"), false);
            manifest.insert(QStringLiteral("error"), diagnosticError);
        }
        const bool manifestSaved =
            writeFile(QDir(result.directory).filePath(QStringLiteral("publish-manifest.json")),
                      QJsonDocument(manifest).toJson(QJsonDocument::Indented), &diagnosticError);
        if (!logSaved || !manifestSaved) {
            if (!result.error.isEmpty())
                result.error += '\n';
            result.error += diagnosticError;
            return false;
        }
        return true;
    };
    auto failPublished = [&](const QString &message) {
        result.error = message;
        manifest.insert(QStringLiteral("error"), message);
        manifest.insert(QStringLiteral("success"), false);
        saveDiagnostics();
        return result;
    };

    QStringList excluded;
#ifdef Q_OS_MACOS
    if (!copyTree(sourceBundle, QDir(result.directory).filePath(relativeBundle), sourceBundle,
                  QDir(result.directory).filePath(relativeBundle), &excluded, &error))
        return failPublished(error);
#elif defined(Q_OS_WIN)
    const QString targetExecutable = QDir(result.directory).filePath(relativeExecutable);
    if (!QFile::copy(executableInfo.absoluteFilePath(), targetExecutable))
        return failPublished(QStringLiteral("Cannot copy the source executable."));
#endif
    manifest.insert(QStringLiteral("excludedBundleArtifacts"), QJsonArray::fromStringList(excluded));
    manifest.insert(QStringLiteral("deployedPlugins"), QJsonArray::fromStringList(selectedPlugins));
    if (!QDir().mkpath(QDir(result.directory).filePath(QStringLiteral("data"))) ||
        !saveProject(runtimeProject, QDir(result.directory).filePath(QStringLiteral("project.ivproj")),
                     &error))
        return failPublished(QStringLiteral("Cannot save runtime project: ") + error);

#ifdef Q_OS_MACOS
    const QString deployedPluginRoot =
        QDir(result.directory).filePath(relativeBundle + QStringLiteral("/Contents/PlugIns"));
    const QString qtConfiguration =
        QDir(result.directory).filePath(relativeBundle + QStringLiteral("/Contents/Resources/qt.conf"));
    const QByteArray qtConfigurationBytes = "[Paths]\nPlugins = PlugIns\n";
#else
    const QString deployedPluginRoot = result.directory;
    const QString qtConfiguration = QDir(result.directory).filePath(QStringLiteral("qt.conf"));
    const QByteArray qtConfigurationBytes = "[Paths]\nPrefix = .\nPlugins = .\n";
#endif
    for (const auto &plugin : selectedPlugins) {
        const QString target = QDir(deployedPluginRoot).filePath(plugin);
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) ||
            !copyTree(QDir(sourcePluginRoot).filePath(plugin), target, sourcePluginRoot, deployedPluginRoot,
                      &excluded, &error))
            return failPublished(QStringLiteral("Cannot copy selected Qt plugin %1: %2").arg(plugin, error));
    }
    if (!QDir().mkpath(QFileInfo(qtConfiguration).absolutePath()) ||
        !writeFile(qtConfiguration, qtConfigurationBytes, &error))
        return failPublished(error);

    QStringList deploymentArguments;
#ifdef Q_OS_MACOS
    deploymentArguments = {QDir(result.directory).filePath(relativeBundle),
                           QStringLiteral("-always-overwrite"), QStringLiteral("-verbose=2"),
                           QStringLiteral("-no-plugins")};
    for (const auto &plugin : selectedPlugins)
        deploymentArguments.append(QStringLiteral("-executable=") +
                                   QDir(deployedPluginRoot).filePath(plugin));
#elif defined(Q_OS_WIN)
    deploymentArguments = {QStringLiteral("--release"),
                           QStringLiteral("--force"),
                           QStringLiteral("--no-plugins"),
                           QStringLiteral("--dir"),
                           result.directory,
                           QDir(result.directory).filePath(relativeExecutable)};
#endif
    manifest.insert(QStringLiteral("deploymentArguments"), QJsonArray::fromStringList(deploymentArguments));
    log += "Selected plugins: " +
           QJsonDocument(QJsonArray::fromStringList(selectedPlugins)).toJson(QJsonDocument::Compact) + '\n';
    log += "Program: " + deploymentTool.toUtf8() + '\n';
    log += "Arguments: " +
           QJsonDocument(QJsonArray::fromStringList(deploymentArguments)).toJson(QJsonDocument::Compact) +
           '\n';
    const auto deployment = runProcess(deploymentTool, deploymentArguments, result.directory);
    log += deployment.output;
    manifest.insert(QStringLiteral("deploymentExitCode"), deployment.exitCode);
    if (!deployment.success)
        return failPublished(
            QStringLiteral("Qt deployment failed: %1 See deployment.log.").arg(deployment.error));
    if (deployment.output.contains("ERROR:"))
        return failPublished(
            QStringLiteral("Qt deployment reported an error despite a zero exit code. See deployment.log."));

#ifdef Q_OS_MACOS
    const QString pluginRoot =
        QDir(result.directory).filePath(relativeBundle + QStringLiteral("/Contents/PlugIns"));
    const QString sqlPlugin = runtimeProject.database.driver == QLatin1String("QODBC")
                                  ? QStringLiteral("sqldrivers/libqsqlodbc.dylib")
                                  : QStringLiteral("sqldrivers/libqsqlite.dylib");
    if (!QFileInfo::exists(QDir(pluginRoot).filePath(QStringLiteral("platforms/libqcocoa.dylib"))) ||
        !QFileInfo::exists(QDir(pluginRoot).filePath(sqlPlugin)))
        return failPublished(
            QStringLiteral("Deployed Cocoa/database plugins are missing; runtime is incomplete."));
    result.launcher = QDir(result.directory).filePath(QStringLiteral("Run.command"));
    const QByteArray launcher =
        (QStringLiteral("#!/bin/sh\nset -eu\nBASE_DIR=$(CDPATH= cd -- \"$(dirname -- \"$0\")\" && pwd)\ncd "
                        "\"$BASE_DIR\"\nexec \"$BASE_DIR\"/") +
         shellLiteral(relativeExecutable) +
         QStringLiteral(
             " --runtime --project \"$BASE_DIR/project.ivproj\" --workspace \"$BASE_DIR\" \"$@\"\n"))
            .toUtf8();
#elif defined(Q_OS_WIN)
    const QString sqlPlugin = runtimeProject.database.driver == QLatin1String("QODBC")
                                  ? QStringLiteral("sqldrivers/qsqlodbc.dll")
                                  : QStringLiteral("sqldrivers/qsqlite.dll");
    if (!QFileInfo::exists(QDir(result.directory).filePath(QStringLiteral("platforms/qwindows.dll"))) ||
        !QFileInfo::exists(QDir(result.directory).filePath(sqlPlugin)))
        return failPublished(
            QStringLiteral("Deployed Windows/database plugins are missing; runtime is incomplete."));
    result.launcher = QDir(result.directory).filePath(QStringLiteral("Run.cmd"));
    const QByteArray launcher =
        "@echo off\r\nsetlocal\r\nset \"IVS_RUNTIME_DIR=%~dp0\"\r\ncd /d "
        "\"%IVS_RUNTIME_DIR%\"\r\n\"%IVS_RUNTIME_DIR%IndustrialVisionStudio.exe\" --runtime --project "
        "\"%IVS_RUNTIME_DIR%project.ivproj\" --workspace \"%IVS_RUNTIME_DIR%.\" %*\r\n";
#else
    const QByteArray launcher;
#endif
    if (!writeFile(result.launcher, launcher, &error))
        return failPublished(error);
#ifdef Q_OS_UNIX
    if (!QFile::setPermissions(result.launcher, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                    QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                                    QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                    QFileDevice::ExeOther))
        return failPublished(QStringLiteral("Cannot make the runtime launcher executable."));
#endif
    const QString databaseDescription =
        runtimeProject.database.driver == QLatin1String("QODBC")
            ? QStringLiteral("The configured SQL Server connection is preserved. Set its password "
                             "environment variable on the target machine and install its ODBC driver.")
            : QStringLiteral("A fresh SQLite database is created at data/inspection.sqlite on first use.");
    const QByteArray readme =
        QStringLiteral(
            "# Industrial Vision Studio Runtime\n\n"
            "Start with %1. The project is project.ivproj; the workspace is this directory.\n"
            "The desktop application also detects this adjacent runtime project when opened normally.\n"
            "%3\n"
            "Production records, captured images and environment passwords are not included.\n\n"
            "The Simulation Console remains available in runtime mode for simulated devices.\n"
            "Use --simulation-console to open it at startup. Virtual state is local to each process.\n"
            "仿真控制台：关闭自动触发后点击单次检测，再点击触发一件。手动调整位置和亮度前勾选手动接管。\n\n"
            "Qt deployment completed using Qt %2. See deployment.log and publish-manifest.json.\n"
            "This package targets the current operating system and architecture only.\n"
            "Commercial camera/motion SDK redistributables and licenses are not packaged by this publisher.\n"
            "Optional hardware/algorithm SDK builds require separate runtime/license deployment and "
            "validation.\n"
            "A deployment-tool success does not replace an application launch/inspection acceptance test.\n")
            .arg(QFileInfo(result.launcher).fileName(), toolVersion, databaseDescription)
            .toUtf8();
    if (!writeFile(QDir(result.directory).filePath(QStringLiteral("README.md")), readme, &error))
        return failPublished(error);
    manifest.insert(QStringLiteral("launcher"), QFileInfo(result.launcher).fileName());
    manifest.insert(QStringLiteral("success"), true);
    if (!saveDiagnostics())
        return result;
    result.success = true;
    return result;
}
} // namespace ivs
