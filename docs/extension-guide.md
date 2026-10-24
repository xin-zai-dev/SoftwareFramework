# 扩展与维护指南

## 层次与依赖

`ui → application → domain / infrastructure`。UI通过queued signal/slot操作PipelineWorker；仓库和设备连接在所属线程创建、使用和销毁。历史查询、导出、发布和容量生成使用独立后台任务，不能共享同一个QSqlDatabase对象跨线程调用。

本版适配器是C++静态编译接入，硬件目录用于展示能力与支持状态；尚未实现任意第三方DLL插件的动态发现/加载。不要把候选型号条目当作真实驱动。

## 添加硬件适配

1. 在`src/infrastructure`新增适配器，封装厂商SDK的连接、断线、停止、取帧/命令等生命周期。
2. 修改`DeviceGateway`的创建和分发路径，并将`hardwareCatalog()`对应状态由计划改为真实实现状态。
3. 如添加backend键，同步`validateProject()`允许集合、设备参数校验和JSON兼容规则。
4. CMake使用显式OFF默认选项/SDK路径，不将专有头文件、DLL和授权文件提交为基础依赖。
5. 用具体设备验证Windows x64 ABI、硬触发、故障、取消、资源释放；在支持矩阵填写型号、SDK、固件、OS、编译器和验证日期。

MVS和HALCON目前只提供可选SDK构建入口，尚无真实采集或HALCON算子流程实现。OpenCV启用后走真实OpenCV阈值和包围框测量。

## 添加算法或流程节点

- 在domain新增算法接口和值类型；不要将UI控件或厂商对象作为跨线程结果。
- 更新`supportedNodeTypes()`、`nodeTitle()`、`validateProject()`和`PipelineWorker::executeNext()`，统一节点输入/输出/顺序、超时及取消语义。
- 如算法输出新测量字段，扩展`DetectionRecord`、数据库迁移/查询/CSV、表格模型、`recordField()`和字段绑定选项。
- 明确什么时候提交数据库和输出PLC。只有提交成功才能发出`resultReady`；输出故障不得伪造“未保存”或悄悄自动重发物理动作。

## 页面与数据库

`PanelConfig`持久化类型、标题和字段。`MainWindow::rebuildDashboard()`按配置顺序组装受约束面板；自绘图表只保留最近1,000条。历史库使用每页100条的ID游标分页，不在UI中缓存百万条。

SQLite和SQL Server分别有建表/分页SQL，数据库密码通过命名环境变量读取。若改表结构，应添加显式schema迁移，而不是直接依赖`CREATE TABLE IF NOT EXISTS`改变已有表。

## 回归与发布

运行根CMake的全部CTest。协议回环测试需要本机监听权限。至少保留：非法工程、错误设备路由、取消/超时、落库失败、图像目录搬移、百万条游标查询、CSV和发布相对路径回归。

当前平台发布器会调用Qt部署工具并只选择所需插件；实际已验证macOS默认Qt构建。Windows需要在Windows/MSVC环境构建及部署验证；Linux便携运行包功能当前明确返回不可用。启用商业算法SDK后，部署器不会自动处理授权与所有专有依赖，需要单独完成合法部署方案。
