# 检测记录存储与百万条基准

`RecordRepository` 为 Qt SQL 数据仓库。默认使用 SQLite；SQL Server 使用 QODBC 和独立的 T-SQL 建表、索引、`TOP`、`OUTPUT INSERTED.id`、`COUNT_BIG` 语句。

## 数据与接口约定

- 一次工件检测为一行。字段包括自增 `id`、唯一 `run_id`、工件号、UTC 时间、配方、OK/NG/ERROR 结果、直径/XY 偏移、耗时、图像路径和消息。数据库不存储图像二进制；压力数据不生成图像。
- 时间统一保存为固定长度的 UTC ISO 8601 毫秒字符串，例如 `2026-10-22T04:30:00.125Z`。SQLite 为 TEXT，SQL Server 为 VARCHAR(24)，便于一致的范围比较。
- `append()` 校验必填标识、有效时间、结果枚举、字段长度和有限测量值；只有成功保存后才返回 `true` 并回填 `id`。同一个 `runId` 与同一内容重试会返回已有 ID；同 ID 的不同内容返回错误，不追加记录。
- `query()` 按 `id DESC` 排序，用 `beforeId` 产生 `id < ?` 游标分页，无 OFFSET 深页扫描。默认每页 100，最多 1,000；非正页大小返回错误。单次查询内存随页大小受限。
- 工件号为**完整标识精确匹配**；结果为 OK/NG/ERROR 或空；日期起止均包含边界，转换为 UTC 后比较。`count()` 使用这些筛选和游标，忽略页大小；查询总条数时将 `beforeId` 设为 0。`summary()` 返回整个数据库的分类汇总。
- 主键、唯一运行 ID，以及 `(workpiece_id,id DESC)`、`(outcome,id DESC)`、`(timestamp_utc,id DESC)` 索引随数据库初始化创建。多条件查询由数据库优化器选择索引，日期范围组合条件的耗时仍取决于命中数据量。
- 每个仓库在调用 `open()` 的线程拥有一个独立连接。执行查询、写入和正常析构应在该线程；跨线程调用被拒绝。跨线程析构将清理排入所属线程，所属线程结束也会关闭并移除连接。不要在一个工作线程结束后继续使用它的仓库。

SQLite 相对路径从工作目录参数 `workspace` 解析，父目录自动建立。默认 `data/inspection.sqlite`；使用 WAL、5 秒忙等待和 `synchronous=NORMAL`。NORMAL 允许在断电时丢失最近提交的事务；需要更强持久性时，应评估存储介质和 FULL 同步策略。打开失败、损坏数据库、未知驱动、SQL 错误会返回具体错误，不视为已保存。

## CSV

CSV 使用 UTF-8、CRLF 记录分隔符、双引号转义和原子文件提交。按 1,000 行游标页流式写出，导出全部匹配项，不受界面当前页的 100 条限制。工件号、配方、消息等文本单元格若包含表格公式前缀（含前置空白后的 `= + - @`，或制表/换行前缀），先加单引号再进行 CSV 引号转义；数值测量列保留数值格式。导出拒绝覆盖数据库、SQLite journal/WAL/SHM 文件及其符号链接别名。

## SQL Server 接入

工程配置设置 `driver=QODBC`，并配置 `host`、`port`、`database`、`user`、`odbcDriver` 和 `passwordEnvironment`。默认 ODBC 驱动名称为 `ODBC Driver 18 for SQL Server`，默认密码环境变量为 `IVS_SQLSERVER_PASSWORD`。密码通过运行进程环境读取，不写入工程 JSON；诊断中的密码会脱敏。用户为空时使用 ODBC 集成身份认证，需由目标系统配置相应身份。连接默认启用加密，`trustServerCertificate=false`；证书信任由部署环境配置。

目标主机需安装 Qt QODBC 插件及对应 SQL Server ODBC 驱动，账号需有建表/建索引权限（或预先建立同一结构）。SQL Server 表为 `dbo.inspection_records`，ID 为 BIGINT IDENTITY，文本为 Unicode NVARCHAR，测量为 FLOAT。

**本次未提供 SQL Server 实例与凭据，未做真实 SQL Server 联机验收。SQLite 的自动化通过不代表 SQL Server 已验证。** Windows 部署时必须分别验证驱动、证书、权限、断线恢复、事务和并发负载。

## 独立压力工具

根工程构建目标为 `ivs_data_tool`，仅操作显式指定的 SQLite 文件。建议与生产记录使用不同目录。

```sh
./build/ivs_data_tool --database build/benchmark/inspection.sqlite --seed 1000000 --report build/benchmark/report.json
```

`--seed` 是本次追加数量，重复运行会继续追加；0 表示仅统计与查询。最多接受 1,000,000,000，非法参数返回非零退出码。数据按 1,000 行事务批次写入、复用预编译 INSERT，不构建百万行内存列表。合成测量值可复现；每次 seed 使用独立 runId 前缀避免碰撞。取消时回滚尚未提交的批次，已提交批次保留，返回取消错误并报告失败。

JSON 包含追加前后实际行数、追加用时、实际吞吐、COUNT 用时、20 个 100 行游标查询页用时、中间游标查询和结果汇总。工具核对实际增量及汇总；不满足要求返回非零。标准输出也提供同一 JSON。报告路径必须区别于数据库及 SQLite 附属文件；通过路径检查后自动建立缺失的报告父目录，写报告失败也返回非零。

## 本机验证记录（2026-10-22）

环境：macOS arm64、Qt 6.11.2、AppleClang 21、Release，使用当前机器本地磁盘。独立测试 CMake 位于 `build/storage-tests/source`，在主工程其余模块并行开发时使用。

```sh
cmake -S build/storage-tests/source -B build/storage-tests/compiled -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.2/macos" -DCMAKE_BUILD_TYPE=Release
cmake --build build/storage-tests/compiled --parallel 4
./build/storage-tests/compiled/tst_repository
ctest --test-dir build/storage-tests/compiled --output-on-failure
```

先编写真实临时 SQLite 测试并以空实现运行：6 个功能测试按预期失败（open 未实现、错误为空）。实现后为绿色。之后增加数据库符号链接覆盖回归测试，先观测 1 个失败，再实现路径解析保护并恢复绿色。最终直接运行 `tst_repository`：**9 passed, 0 failed**（7 个功能测试加初始化/清理）；CTest：**1/1 passed**。覆盖持久化重开、UTC 时区转换、工件/结果/日期筛选、无重复游标分页、汇总、重复运行与冲突、2,051 行 CSV（超过两个内部页）、公式/引号/换行、最大页限制、数据库别名保护、损坏数据库、非法写入、取消及后续继续追加。主工程整体测试由集成验收另行记录。

```sh
./build/storage-tests/compiled/ivs_data_tool --database build/storage-tests/million.sqlite --seed 1000000 --report build/storage-tests/million-report.json
```

该命令退出码 0，报告 `success=true`：

| 项目 | 实测值 |
| --- | ---: |
| 追加前 / 后实际行数 | 0 / 1,000,000 |
| 追加耗时 | 5,884.116 ms |
| 追加吞吐 | 169,949 行/秒 |
| 20 个 100 行页平均 / 最大耗时 | 0.092829 / 0.150959 ms |
| 中间游标 `beforeId=500000`，100 行 | 0.091583 ms |
| COUNT 耗时 | 12.4105 ms |
| OK / NG / ERROR | 900,000 / 50,000 / 50,000 |
| SQLite 主数据库大小 | 332,595,200 字节 |

另使用 Python SQLite 打开实际文件确认 `COUNT(*)=1000000`、`PRAGMA quick_check=ok`，并确认三个筛选索引和唯一运行索引存在；游标执行计划为 `SEARCH inspection_records USING INTEGER PRIMARY KEY (rowid<?)`。

```sh
./build/storage-tests/compiled/ivs_data_tool --database build/storage-tests/invalid.sqlite --seed wrong --report build/storage-tests/invalid-report.json
```

该非法参数命令退出码 2，报告 `success=false`，没有创建记录数据库。百万条结果反映本机合成数据和本地缓存下的测量，不是外部 SQL Server、真实相机吞吐、冷缓存或生产硬件的性能承诺。基准数据库位于 build 目录，不随发布工程默认带出。

### 报告目录回归

主工程集成时，命令 `--database data/benchmark.sqlite --seed 1000000 --report reports/million-records.json` 已实际完成 0→1,000,000 行追加，记录追加耗时 5,725.444542 ms、平均查询页 0.09263335 ms；随后因为 `reports/` 不存在而写报告失败，退出码为 1。此类报告失败不回滚已经提交的记录，因此重新生成报告应使用 `--seed 0`，避免再追加一百万条。

先使用 3 行独立测试库复现相同问题：缺失的多级报告目录导致命令退出码 1。修复后执行以下真实命令：

```sh
./build/storage-tests/compiled/ivs_data_tool --database build/storage-tests/report-regression.sqlite --seed 0 --report build/storage-tests/report-regression/new/nested/report.json
```

退出码为 0，自动创建此前不存在的多级目录。随后读取输出 JSON 并断言 `success=true`、`rowsBefore=3`、`rowsAfter=3`、`seededRows=0`，验证报告落盘且未重复追加记录。数据库/报告路径及符号链接别名检查仍先于目录创建执行。
