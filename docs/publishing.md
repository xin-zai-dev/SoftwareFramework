# 客户运行目录发布

`ivs::publishRuntime(project, destination, sourceExecutable, qtBinDirectory)` 是同步发布接口，返回 `PublishResult { success, directory, launcher, error }`。界面通过后台任务调用，避免部署工具和文件复制阻塞主线程。

## 输出

发布目标必须是**尚不存在的新目录**，已有空目录也不复用。发布不会覆盖已有运行包；无效工程、缺失可执行文件、目标位于源 `.app` 内部、工具缺失或 Qt 版本不符均拒绝。进程通过 QProcess 的程序和参数列表调用，不拼接 shell 命令。

macOS 输出示例：

```text
customer-runtime/
  IndustrialVisionStudio.app/
  Run.command
  project.ivproj
  data/
  README.md
  deployment.log
  publish-manifest.json
```

Windows 使用 `IndustrialVisionStudio.exe` 和 `Run.cmd`，Qt DLL 及 plugins 由 windeployqt 收集。目标平台和架构与当前运行程序一致；macOS 不能直接生成 Windows 二进制。

启动器定位自身所在目录，传入 `--runtime --project <本目录>/project.ivproj --workspace <本目录>`，可将整个目录移动到其他位置。主程序正常打开时也从邻接目录检测运行工程：macOS 为 `.app` 所在目录，Windows 为 exe 所在目录。

## 数据隔离

发布保存当前经过校验的配置、流程、设备和页面，并保留选定的数据库后端。SQLite 只将文件路径改为相对的 `data/inspection.sqlite`，首次运行时创建新数据库。QODBC 保留 SQL Server 服务器、端口、数据库名、用户、ODBC 驱动、证书设置和密码环境变量名称；目标机器需配置相应环境变量和 ODBC 驱动。发布不查询、复制数据库内容或生产工作目录，也不读取或保存密码环境变量值。

设备与节点参数中命名为 password/passwd/pwd/secret/token/accessToken/apiKey/credential(s)/connectionString/dsn 的字段递归移除（大小写、下划线和连字符归一化），其余参数保留。发布工程不包含生产图像。

macOS 仅复制应用 bundle，不复制其同级工作目录。bundle 中的 data、captures、inspection-images、workspace、logs、`.git`、`.codex`、`.env*`，数据库及 journal/WAL/SHM、旧 `.ivproj`，以及常见运行图像目录会排除并记录在 manifest。程序图标与 Qt 自身资源保留。应用 bundle 中不应嵌入自定义命名的生产资料；发布不能识别任意资源内容的业务含义。

## 依赖部署与可用性

- macOS：源程序必须位于 `.app` 内；递归复制目录和文件，保留 framework 的内部相对符号链接。内部绝对链接改写为包内相对链接；断裂链接和指向 bundle 外部的链接拒绝。复制目录不跟随链接递归，因此不会把外部目录或目标目录递归带入。
- Qt 部署工具从显式 `qtBinDirectory` 或当前 Qt 的 BinariesPath 查找。相邻 qmake 的 `-query QT_VERSION` 必须与当前 `qVersion()` 一致，防止混用 Qt 安装。
- 插件采用显式选择，不扫描整个 Qt SQL 驱动目录。基础包包含当前平台插件及 SQLite；工程选择 QODBC 时额外包含 QODBC。安装中可用的 JPEG 和 offscreen 插件会一起带出，后者用于无桌面验证。Postgres、Mimer 等无关驱动不带出。插件来源通过已核对版本的 qmake `QT_INSTALL_PLUGINS` 查询，重新发布已部署 bundle 时也不保留旧插件目录。
- macOS 实际运行 `macdeployqt <新应用包> -always-overwrite -verbose=2 -no-plugins`，并将每个已复制的插件以独立 `-executable=<插件路径>` 参数传入，让工具收集并改写其 Qt framework 依赖。Windows 在 `windeployqt --help` 确认支持后使用 `--no-plugins`，手动复制同一选择策略的插件。两者都写入相对 Qt 插件配置 `qt.conf`。工具启动失败、超时、非零退出，或 macdeployqt 输出 ERROR，均视为失败。成功后还检查平台插件及所选数据库插件存在。QODBC 插件不替代目标系统上的 SQL Server ODBC 驱动与认证配置。
- Linux 当前没有可验证的可移植依赖收集路径，接口返回明确的不可用错误。Linux 应使用经过验证的 CMake install/package 流程；本实现不把裸 exe 复制称为独立运行包。
- 商业相机、运动与算法 SDK 的运行库和授权不由此发布器再分发。macOS 在部署前检查主程序依赖，发现可选非 Qt/非系统动态库时拒绝，要求使用单独验证的部署流程；bundle 中已知商业 SDK 文件也拒绝复制。启用 OpenCV/HALCON/MVS 等额外构建依赖时须单独处理其运行库和授权，不能根据默认无 SDK 构建的验收推断已支持。

`publish-manifest.json` 记录 Qt 和部署工具版本、实际部署参数、退出码、相对可执行文件和工程路径、排除项及 success。`deployment.log` 保存实际工具命令和合并输出。诊断可包含部署工具的开发安装路径；运行启动器和工程不依赖该路径。

若在创建目标之后失败，目录保留为诊断材料，manifest 标记 `success=false`，界面收到失败结果；不得将其视为可交付运行包。重新发布需另选新目录。工具成功仅表示依赖部署步骤完成，交付前还需实际启动、模拟检测与数据库写入验收；Windows 分支尚需在 Windows 主机上验证。

## 本次自动化验证（2026-10-22）

macOS arm64、Qt 6.11.2、Release；先编写测试并使用空实现运行，3 个测试按预期失败（缺少相应错误报告）。实现后执行：

```sh
cmake -S build/publisher-tests/source -B build/publisher-tests/compiled -DCMAKE_PREFIX_PATH="$HOME/Qt/6.11.2/macos" -DCMAKE_BUILD_TYPE=Release
cmake --build build/publisher-tests/compiled --parallel 4
./build/publisher-tests/compiled/tst_publisher
ctest --test-dir build/publisher-tests/compiled --output-on-failure
```

最初三个功能测试通过后，针对 SQL Server 发布误切换 SQLite 的评审反馈增加运行配置回归测试，先观测 QODBC 被改成 QSQLITE 的失败，再修正为只重写 SQLite 路径。随后增加插件选择回归，先观测必需插件缺失的失败，再实现只包含所需 SQL 驱动的策略。最终直接 QtTest 为 **8 passed, 0 failed**（6 个功能测试加初始化/清理）；独立 CTest **1/1 passed**。配置测试核对 QODBC 所有连接配置及序列化结果保留、SQLite 路径相对化、嵌套密码/令牌字段移除、原工程不被修改。已有目标测试确认原文件内容保持不变，失败路径测试确认没有新建运行目录。

### 真实 macOS 发布与移动目录运行

首次真实发布揭示 Qt 6.11.2 的默认 SQL 插件部署会连带复制 Postgres/Mimer/QODBC，进而访问本机不存在的 Postgres、Mimer、iODBC 库。macdeployqt 虽退出 0，日志仍有 ERROR，发布器按预期返回失败。修复采用上述显式插件策略，保留错误检测，不忽略缺失库。

```sh
cmake --build build/macos --target IndustrialVisionStudio --parallel 4
QT_QPA_PLATFORM=offscreen ./build/macos/IndustrialVisionStudio.app/Contents/MacOS/IndustrialVisionStudio --publish-to verification/publish-agent
```

构建和发布均退出 0，manifest 为 `success=true`，deployment.log 无 ERROR。检查新包 `data/` 为空，插件恰为 Cocoa、SQLite、JPEG、offscreen；未带出 Postgres、Mimer 或 QODBC。检查主程序、插件和 framework 共 12 个 Mach-O 的动态库引用，无 `/Users` 开发 Qt、Postgres、Homebrew 或 `/usr/local` 依赖。

随后将整个目录移动为 `verification/publish-agent-relocated`，取消外部 Qt/DYLD 库路径设置并通过生成的启动器执行：

```sh
env -u QT_PLUGIN_PATH -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH QT_QPA_PLATFORM=offscreen ./verification/publish-agent-relocated/Run.command --smoke-test --cycles 3
codesign --verify --deep --strict verification/publish-agent-relocated/IndustrialVisionStudio.app
```

启动器和签名检查均退出 0。模拟验收报告位于移动后目录的 `reports/smoke.json`：`success=true`、3 个周期、`rowsAdded=3`，测量 20.000 mm，实际创建并写入新的 SQLite 数据库。此验证目录因验收运行已包含三条记录，不应作为无数据交付包；正式交付需发布到另一个新目录。这里验证了 macOS 无桌面运行，正常 Cocoa 窗口和 Windows 交付仍需各自在目标环境验收。
