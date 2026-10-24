#pragma once

#include "domain/Types.h"
#include <QStringList>

namespace ivs {

struct PublishResult {
    bool success = false;
    QString directory;
    QString launcher;
    QString error;
};

// Prepare exportable settings while retaining the selected database backend.
ProjectConfig runtimeProjectForPublishing(const ProjectConfig &project);
QStringList requiredRuntimePlugins(const DatabaseConfig &database);

// Synchronous filesystem/deployment work; invoke from a worker thread.
PublishResult publishRuntime(const ProjectConfig &project, const QString &destination,
                             const QString &sourceExecutable, const QString &qtBinDirectory = {});

} // namespace ivs
