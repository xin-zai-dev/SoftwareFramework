#pragma once
#include "domain/Types.h"
#include <QStringList>

namespace ivs {
ProjectConfig defaultProject();
QStringList validateProject(const ProjectConfig &project);
QJsonObject projectToJson(const ProjectConfig &project);
bool projectFromJson(const QJsonObject &json, ProjectConfig *project, QString *error);
bool saveProject(const ProjectConfig &project, const QString &path, QString *error);
bool loadProject(const QString &path, ProjectConfig *project, QString *error);
QString nodeTitle(const QString &type);
QString fieldTitle(const QString &field);
QStringList supportedNodeTypes();
QStringList supportedPanelTypes();
} // namespace ivs
