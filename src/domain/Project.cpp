#include "domain/Project.h"
#include "domain/RobotKinematics.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <cmath>
#include <limits>

namespace ivs {
QStringList supportedNodeTypes() {
    return {"trigger", "move",   "light", "capture",    "inspect",    "judge",
            "store",   "output", "delay", "robot_move", "robot_grip", "robot_sort"};
}
QStringList supportedPanelTypes() {
    return {"image", "table", "trend", "scatter", "log"};
}
QString nodeTitle(const QString &type) {
    static const QHash<QString, QString> names{
        {"trigger", "等待触发"},      {"move", "运动定位"},         {"light", "光源控制"},
        {"capture", "图像采集"},      {"inspect", "图像测量"},      {"judge", "结果判定"},
        {"store", "保存记录"},        {"output", "输出结果"},       {"delay", "延时等待"},
        {"robot_move", "机械臂定位"}, {"robot_grip", "机械臂夹爪"}, {"robot_sort", "视觉分拣"}};
    return names.value(type, type);
}
QString fieldTitle(const QString &field) {
    static const QHash<QString, QString> names{{"diameterMm", "直径 (mm)"},
                                               {"offsetXmm", "X 偏移 (mm)"},
                                               {"offsetYmm", "Y 偏移 (mm)"},
                                               {"durationMs", "检测耗时 (ms)"}};
    return names.value(field, field);
}
ProjectConfig defaultProject() {
    ProjectConfig p;
    p.devices = {{"camera-1", "camera", "模拟相机", "simulated"},
                 {"motion-1", "motion", "模拟四轴平台", "simulated"},
                 {"light-1", "light", "模拟四通道光源", "simulated"},
                 {"plc-1", "plc", "模拟 Modbus PLC", "simulated"},
                 {"io-1", "io", "模拟数字 I/O", "simulated"},
                 {"robot-1", "robot", "SCARA 四轴仿真机械臂", "simulated"}};
    for (const auto &type :
         QStringList{"trigger", "move", "light", "capture", "inspect", "judge", "store", "output"}) {
        FlowNode node{type + "-1", type, nodeTitle(type), true, {}};
        if (type == "trigger")
            node.parameters = {{"timeoutMs", 60000}};
        if (type == "move")
            node.parameters = {{"axis", 0}, {"position", 0}, {"speed", 25}};
        if (type == "light")
            node.parameters = {{"channel", 0}, {"brightness", 80}, {"enabled", true}};
        p.flow.append(node);
    }
    p.panels = {{"image-1", "image", "实时检测图像", "diameterMm"},
                {"table-1", "table", "最近检测记录", "diameterMm"},
                {"trend-1", "trend", "尺寸趋势", "diameterMm"},
                {"scatter-1", "scatter", "位置分布", "offsetYmm"},
                {"log-1", "log", "运行事件", "durationMs"}};
    return p;
}
QStringList validateProject(const ProjectConfig &p) {
    QStringList errors;
    if (p.schemaVersion != 1)
        errors << "不支持的工程版本";
    if (p.name.trimmed().isEmpty() || p.name.size() > 200)
        errors << "工程名称不能为空且不能超过200字符";
    if (p.resolution.width() < 64 || p.resolution.height() < 64 || p.resolution.width() > 8192 ||
        p.resolution.height() > 8192 || qint64(p.resolution.width()) * p.resolution.height() > 16777216)
        errors << "图像尺寸需至少64×64，最多16777216像素，单边不超过8192";
    if (p.cycleMs < 100 || p.cycleMs > 60000)
        errors << "节拍必须在100至60000毫秒之间";
    if (!std::isfinite(p.pixelsPerMm) || p.pixelsPerMm <= 0 || p.pixelsPerMm > 100000)
        errors << "像素标定比例必须为正有限数";
    if (!std::isfinite(p.minDiameterMm) || !std::isfinite(p.maxDiameterMm) || p.minDiameterMm < 0 ||
        p.maxDiameterMm <= p.minDiameterMm)
        errors << "直径上下限无效";
    if (!QStringList{"none", "ng", "all", "sample"}.contains(p.imagePolicy))
        errors << "未知图像保留策略";
    if (p.imageRetentionDays < 1 || p.imageRetentionDays > 3650 || p.imageQuotaMB < 1 ||
        p.imageQuotaMB > 1000000)
        errors << "图像保留期限或容量无效";
    if (!QStringList{"QSQLITE", "QODBC"}.contains(p.database.driver) ||
        p.database.database.trimmed().isEmpty())
        errors << "数据库类型或数据库名称无效";
    if (p.database.port < 1 || p.database.port > 65535)
        errors << "数据库端口无效";
    QSet<QString> deviceIds;
    for (const auto &d : p.devices) {
        if (d.id.isEmpty() || deviceIds.contains(d.id))
            errors << "设备标识为空或重复";
        deviceIds.insert(d.id);
        if (!QStringList{"camera", "motion", "light", "plc", "io", "robot"}.contains(d.category))
            errors << "未知设备类型: " + d.category;
        if (!QStringList{"simulated", "modbus-tcp", "mvs", "baumer", "googol", "leisai", "opt", "ccs"}
                 .contains(d.backend))
            errors << "未知设备后端: " + d.backend;
        if (d.port < 1 || d.port > 65535 || d.unitId < 0 || d.unitId > 247)
            errors << "设备端口或单元地址无效";
        if (d.category == "robot" && d.backend != "simulated")
            errors << "机械臂只支持 simulated 后端";
        if (d.backend == "modbus-tcp" && d.category != "plc" && d.category != "io")
            errors << "Modbus TCP 后端仅用于 PLC / I/O";
    }
    if (p.flow.size() > 128)
        errors << "流程节点不能超过128个";
    QSet<QString> nodeIds;
    QHash<QString, int> indices, counts;
    int active = 0;
    for (const auto &n : p.flow) {
        if (n.id.isEmpty() || nodeIds.contains(n.id))
            errors << "流程节点标识为空或重复";
        nodeIds.insert(n.id);
        if (!supportedNodeTypes().contains(n.type))
            errors << "不支持的节点: " + n.type;
        if (!n.enabled)
            continue;
        ++counts[n.type];
        indices[n.type] = active++;
        if (n.parameters.contains("deviceId") &&
            (!n.parameters.value("deviceId").isString() ||
             n.parameters.value("deviceId").toString().trimmed().isEmpty()))
            errors << "deviceId 必须为非空字符串，省略时才使用首个匹配设备";
        const auto deviceId = n.parameters.value("deviceId").toString();
        if (!deviceId.isEmpty() && !deviceIds.contains(deviceId))
            errors << "节点引用不存在的设备: " + deviceId;
        if (n.type == "robot_move" || n.type == "robot_grip" || n.type == "robot_sort") {
            const DeviceConfig *robot = nullptr;
            for (const auto &device : p.devices)
                if ((!deviceId.isEmpty() && device.id == deviceId) ||
                    (deviceId.isEmpty() && device.category == "robot")) {
                    robot = &device;
                    break;
                }
            if (!robot || robot->category != "robot" || robot->backend != "simulated")
                errors << "机械臂节点必须选择 simulated 机械臂设备";
            QSet<QString> allowed{"deviceId", "timeoutMs"};
            if (n.type == "robot_move")
                allowed.unite({"x", "y", "z", "r", "elbowPositive", "speedPercent"});
            else if (n.type == "robot_grip")
                allowed.insert("closed");
            else
                allowed.unite({"speedPercent", "restock"});
            for (auto field = n.parameters.begin(); field != n.parameters.end(); ++field)
                if (!allowed.contains(field.key()))
                    errors << "机械臂节点不支持参数: " + field.key();
            const auto number = [&](const QString &key, double fallback, double minimum, double maximum,
                                    double *out) {
                const auto value = n.parameters.value(key);
                if (value.isUndefined()) {
                    *out = fallback;
                    return true;
                }
                if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < minimum ||
                    value.toDouble() > maximum) {
                    errors << "机械臂参数 " + key + " 必须为范围内的有限数字";
                    return false;
                }
                *out = value.toDouble();
                return true;
            };
            if (n.type != "robot_grip") {
                double speed = 60;
                number("speedPercent", 60, 1, 100, &speed);
            }
            const QString booleanKey = n.type == "robot_move"   ? "elbowPositive"
                                       : n.type == "robot_grip" ? "closed"
                                                                : "restock";
            if (n.parameters.contains(booleanKey) && !n.parameters.value(booleanKey).isBool())
                errors << "机械臂参数 " + booleanKey + " 必须为布尔值";
            if (n.type == "robot_move") {
                RobotPose target{220, 40, 100, 0};
                const double bound = std::numeric_limits<double>::max();
                const bool validX = number("x", 220, -bound, bound, &target.x);
                const bool validY = number("y", 40, -bound, bound, &target.y);
                const bool validZ = number("z", 100, 0, 150, &target.z);
                const bool validR = number("r", 0, -bound, bound, &target.r);
                RobotJoints joints;
                QString kinematicsError;
                if (validX && validY && validZ && validR &&
                    !robotInverse(target, n.parameters.value("elbowPositive").toBool(true), &joints,
                                  &kinematicsError))
                    errors << "机械臂目标不可达: " + kinematicsError;
            }
        }
        if (n.type == "delay") {
            auto v = n.parameters.value("ms");
            if (!v.isDouble() || v.toDouble() < 0 || v.toDouble() > 30000 ||
                std::floor(v.toDouble()) != v.toDouble())
                errors << "延时节点 ms 必须是0至30000的整数";
        }
        for (const auto &key : QStringList{"position", "speed", "axis", "channel", "brightness", "address",
                                           "value", "requireValue"}) {
            if (n.parameters.contains(key) &&
                (!n.parameters.value(key).isDouble() || !std::isfinite(n.parameters.value(key).toDouble())))
                errors << "节点参数 " + key + " 必须为有限数字";
        }
        if (n.parameters.contains("address") && (n.parameters.value("address").toDouble() < 0 ||
                                                 n.parameters.value("address").toDouble() > 65535 ||
                                                 std::floor(n.parameters.value("address").toDouble()) !=
                                                     n.parameters.value("address").toDouble()))
            errors << "Modbus 地址必须为0至65535的整数";
        if (n.type == "move") {
            if (n.parameters.contains("axis")) {
                const auto axis = n.parameters.value("axis").toDouble(-1);
                if (axis < 0 || axis > 3 || std::floor(axis) != axis)
                    errors << "运动轴号必须为0至3的整数";
            }
            if (n.parameters.contains("speed")) {
                const auto speed = n.parameters.value("speed").toDouble(-1);
                if (speed <= 0 || speed > 1000)
                    errors << "运动速度必须大于0且不超过1000 mm/s";
            }
        }
        if (n.type == "light") {
            if (n.parameters.contains("channel")) {
                const auto channel = n.parameters.value("channel").toDouble(-1);
                if (channel < 0 || channel > 3 || std::floor(channel) != channel)
                    errors << "光源通道必须为0至3的整数";
            }
            if (n.parameters.contains("enabled") && !n.parameters.value("enabled").isBool())
                errors << "光源 enabled 参数必须为布尔值";
        }
        if (n.parameters.contains("brightness")) {
            const auto brightness = n.parameters.value("brightness").toDouble(-1);
            if (brightness < 0 || brightness > 255 || std::floor(brightness) != brightness)
                errors << "亮度必须为0至255的整数";
        }
        for (const auto &key : QStringList{"value", "requireValue"})
            if (n.parameters.contains(key)) {
                const auto value = n.parameters.value(key).toDouble(-1);
                if (value < 0 || value > 65535 || std::floor(value) != value)
                    errors << "参数 " + key + " 必须为0至65535的整数";
            }
        if (n.parameters.contains("function")) {
            const auto function = n.parameters.value("function");
            const QStringList functions = n.type == "output"
                                              ? QStringList{"register", "holding", "coil"}
                                              : QStringList{"holding", "input", "coil", "discrete"};
            if (!function.isString() || !functions.contains(function.toString()))
                errors << "Modbus function 参数无效";
        }
        for (const auto &key : QStringList{"timeoutMs", "pollMs"})
            if (n.parameters.contains(key)) {
                const auto value = n.parameters.value(key);
                if (!value.isDouble() || value.toDouble() < 1 || value.toDouble() > 60000 ||
                    std::floor(value.toDouble()) != value.toDouble())
                    errors << "触发时序参数 " + key + " 必须为1至60000的整数";
            }
    }
    for (const auto &type : QStringList{"capture", "inspect", "judge", "store"})
        if (counts.value(type) != 1)
            errors << "必须且只能有一个启用的" + nodeTitle(type) + "节点";
    if (counts.value("capture") == 1 && counts.value("inspect") == 1 &&
        indices["inspect"] < indices["capture"])
        errors << "测量必须在采集之后";
    if (counts.value("inspect") == 1 && counts.value("judge") == 1 && indices["judge"] < indices["inspect"])
        errors << "判定必须在测量之后";
    if (counts.value("judge") == 1 && counts.value("store") == 1 && indices["store"] < indices["judge"])
        errors << "保存必须在判定之后";
    if (counts.value("output") > 1 ||
        (counts.value("output") == 1 && indices["output"] < indices.value("store")))
        errors << "结果输出只能有一个且必须在保存之后";
    if (counts.value("robot_sort") > 1 ||
        (counts.value("robot_sort") == 1 && indices["robot_sort"] < indices.value("store")))
        errors << "视觉分拣只能有一个且必须在保存之后";
    for (const auto &type : QStringList{"trigger", "move", "light"})
        if (counts.value(type) && indices[type] > indices.value("capture"))
            errors << nodeTitle(type) + "必须在采集之前";
    QSet<QString> panelIds;
    if (p.panels.isEmpty() || p.panels.size() > 12)
        errors << "页面需包含1至12个面板";
    for (const auto &panel : p.panels) {
        if (panel.id.isEmpty() || panelIds.contains(panel.id))
            errors << "面板标识为空或重复";
        panelIds.insert(panel.id);
        if (!supportedPanelTypes().contains(panel.type))
            errors << "未知面板类型: " + panel.type;
        if (!QStringList{"diameterMm", "offsetXmm", "offsetYmm", "durationMs"}.contains(panel.field))
            errors << "未知数据绑定字段: " + panel.field;
    }
    return errors;
}
QJsonObject projectToJson(const ProjectConfig &p) {
    QJsonArray devices, flow, panels;
    for (const auto &d : p.devices)
        devices.append(QJsonObject{{"id", d.id},
                                   {"category", d.category},
                                   {"model", d.model},
                                   {"backend", d.backend},
                                   {"host", d.host},
                                   {"port", d.port},
                                   {"unitId", d.unitId},
                                   {"parameters", d.parameters}});
    for (const auto &n : p.flow)
        flow.append(QJsonObject{{"id", n.id},
                                {"type", n.type},
                                {"title", n.title},
                                {"enabled", n.enabled},
                                {"parameters", n.parameters}});
    for (const auto &panel : p.panels)
        panels.append(QJsonObject{
            {"id", panel.id}, {"type", panel.type}, {"title", panel.title}, {"field", panel.field}});
    QJsonObject db{{"driver", p.database.driver},
                   {"database", p.database.database},
                   {"host", p.database.host},
                   {"port", p.database.port},
                   {"user", p.database.user},
                   {"passwordEnvironment", p.database.passwordEnvironment},
                   {"odbcDriver", p.database.odbcDriver},
                   {"trustServerCertificate", p.database.trustServerCertificate}};
    return {{"schemaVersion", 1},
            {"name", p.name},
            {"recipe", p.recipe},
            {"resolution", QJsonObject{{"width", p.resolution.width()}, {"height", p.resolution.height()}}},
            {"cycleMs", p.cycleMs},
            {"pixelsPerMm", p.pixelsPerMm},
            {"minDiameterMm", p.minDiameterMm},
            {"maxDiameterMm", p.maxDiameterMm},
            {"imagePolicy", p.imagePolicy},
            {"imageRetentionDays", p.imageRetentionDays},
            {"imageQuotaMB", double(p.imageQuotaMB)},
            {"database", db},
            {"devices", devices},
            {"flow", flow},
            {"panels", panels}};
}
bool projectFromJson(const QJsonObject &json, ProjectConfig *out, QString *error) {
    const auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!out)
        return fail("工程目标为空");
    if (json.value("schemaVersion").toDouble(-1) != 1)
        return fail("无法读取：工程版本不受支持");
    if (!json.value("name").isString() || !json.value("resolution").isObject() ||
        !json.value("flow").isArray() || !json.value("devices").isArray() ||
        !json.value("panels").isArray() || !json.value("database").isObject())
        return fail("工程缺少必要字段或字段类型无效");
    ProjectConfig p;
    p.name = json.value("name").toString();
    p.recipe = json.value("recipe").toString();
    const auto size = json.value("resolution").toObject();
    const auto integer = [](const QJsonValue &v) {
        return v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble();
    };
    const auto optionalInt = [&](const QJsonObject &object, const QString &key, double minimum,
                                 double maximum) {
        if (!object.contains(key))
            return true;
        const auto v = object.value(key);
        return integer(v) && v.toDouble() >= minimum && v.toDouble() <= maximum;
    };
    const auto optionalString = [](const QJsonObject &object, const QString &key) {
        return !object.contains(key) || object.value(key).isString();
    };
    if (!integer(size.value("width")) || !integer(size.value("height")) || !integer(json.value("cycleMs")))
        return fail("尺寸和节拍必须为整数");
    p.resolution = QSize(size.value("width").toInt(-1), size.value("height").toInt(-1));
    p.cycleMs = json.value("cycleMs").toInt(-1);
    for (const auto &key : QStringList{"pixelsPerMm", "minDiameterMm", "maxDiameterMm"})
        if (!json.value(key).isDouble())
            return fail("测量参数类型无效: " + key);
    p.pixelsPerMm = json.value("pixelsPerMm").toDouble();
    p.minDiameterMm = json.value("minDiameterMm").toDouble();
    p.maxDiameterMm = json.value("maxDiameterMm").toDouble();
    if (!optionalInt(json, "imageRetentionDays", 1, 3650) || !optionalInt(json, "imageQuotaMB", 1, 1000000) ||
        !optionalString(json, "imagePolicy") || !optionalString(json, "recipe"))
        return fail("图像保留配置或配方类型/范围无效");
    p.imagePolicy = json.value("imagePolicy").toString("ng");
    p.imageRetentionDays = json.value("imageRetentionDays").toInt(30);
    p.imageQuotaMB = qint64(json.value("imageQuotaMB").toDouble(1024));
    const auto db = json.value("database").toObject();
    if (!optionalInt(db, "port", 1, 65535) ||
        (db.contains("trustServerCertificate") && !db.value("trustServerCertificate").isBool()))
        return fail("数据库端口或证书设置类型/范围无效");
    for (const auto &key :
         QStringList{"driver", "database", "host", "user", "passwordEnvironment", "odbcDriver"})
        if (!optionalString(db, key))
            return fail("数据库字段必须为字符串: " + key);
    p.database.driver = db.value("driver").toString();
    p.database.database = db.value("database").toString();
    p.database.host = db.value("host").toString("localhost");
    p.database.port = db.value("port").toInt(1433);
    p.database.user = db.value("user").toString();
    p.database.passwordEnvironment = db.value("passwordEnvironment").toString("IVS_SQLSERVER_PASSWORD");
    p.database.odbcDriver = db.value("odbcDriver").toString("ODBC Driver 18 for SQL Server");
    p.database.trustServerCertificate = db.value("trustServerCertificate").toBool(false);
    for (const auto &v : json.value("devices").toArray()) {
        if (!v.isObject())
            return fail("设备定义不是对象");
        const auto d = v.toObject();
        if (!d.value("parameters").isObject())
            return fail("设备参数不是对象");
        if (!optionalInt(d, "port", 1, 65535) || !optionalInt(d, "unitId", 0, 247))
            return fail("设备端口或Unit ID类型/范围无效");
        for (const auto &key : QStringList{"id", "category", "model", "backend", "host"})
            if (!optionalString(d, key))
                return fail("设备字段必须为字符串: " + key);
        p.devices.append({d.value("id").toString(), d.value("category").toString(),
                          d.value("model").toString(), d.value("backend").toString(),
                          d.value("host").toString("127.0.0.1"), d.value("port").toInt(502),
                          d.value("unitId").toInt(1), d.value("parameters").toObject()});
    }
    for (const auto &v : json.value("flow").toArray()) {
        if (!v.isObject())
            return fail("流程定义不是对象");
        const auto n = v.toObject();
        if (!n.value("parameters").isObject() || !n.value("enabled").isBool())
            return fail("流程参数或启用状态类型无效");
        p.flow.append({n.value("id").toString(), n.value("type").toString(), n.value("title").toString(),
                       n.value("enabled").toBool(), n.value("parameters").toObject()});
    }
    for (const auto &v : json.value("panels").toArray()) {
        if (!v.isObject())
            return fail("面板定义不是对象");
        const auto n = v.toObject();
        p.panels.append({n.value("id").toString(), n.value("type").toString(), n.value("title").toString(),
                         n.value("field").toString()});
    }
    const auto errors = validateProject(p);
    if (!errors.isEmpty())
        return fail(errors.join('\n'));
    *out = p;
    if (error)
        error->clear();
    return true;
}
bool saveProject(const ProjectConfig &p, const QString &path, QString *error) {
    const auto errors = validateProject(p);
    if (!errors.isEmpty()) {
        if (error)
            *error = errors.join('\n');
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = "无法创建工程目录";
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(projectToJson(p)).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (error)
        error->clear();
    return true;
}
bool loadProject(const QString &path, ProjectConfig *out, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.size() > 2 * 1024 * 1024) {
        if (error)
            *error = "工程文件超过2MB限制";
        return false;
    }
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = "JSON工程格式错误: " + parseError.errorString();
        return false;
    }
    return projectFromJson(doc.object(), out, error);
}
} // namespace ivs
