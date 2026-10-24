# Industrial Vision Studio Implementation Plan

> For agentic workers: use isolated file ownership for independent modules and a final independent code review. The human has explicitly requested implementation of the current design in this directory.

**Goal:** Deliver a runnable C++ Qt project with visual sequential workflows, configurable dashboards, persisted inspection data and local runtime publishing.
**Architecture:** Domain types/project validation are shared contracts. UI consumes a worker pipeline and thread-confined repositories. Device abstractions use simulation by default and genuine network/optional SDK adapters where available.
**Tech Stack:** Qt6 Core/Gui/Widgets/Sql/Network/Concurrent/Test, CMake, C++17; OpenCV/HALCON/MVS optional.
**Spec:** docs/design.md

## Global Constraints
- All source, samples, docs, build artifacts and verification reports stay in this project folder.
- Vendor SDK absence must not prevent the default build or be misreported as working hardware support.
- UI does not perform capture/algorithm/record-writing in its main event loop.
- Data capacity is cumulative 1,000,000 inspection records; images have separate retention policy.

## Review Focus
- Invalid node order/parameters must fail validation before hardware execution.
- Stop and window close must not leave active timers/threads or persist phantom successes.
- Pagination and filters must not duplicate/skip matching rows in a stable dataset.
- Publishing must include Qt dependencies, use a relative runtime project and exclude private data.
- Missing SDK, unsupported model and database failure must be observable.

## Tasks
- [x] 1. Define domain contracts and failing validation/repository tests; configure portable build.
- [x] 2. Implement project serialization/validation and native/OpenCV image analysis.
- [x] 3. Implement SQLite/QODBC repository, CSV and million-record benchmark tool with tests.
- [x] 4. Implement simulation, Modbus TCP, hardware catalog and optional SDK integration with tests.
- [x] 5. Implement drag/drop workflow and dashboard editors, image and bounded chart widgets.
- [x] 6. Integrate threaded runtime and complete Chinese desktop UI, persistence, history and publish.
- [x] 7. Build/test, execute one-million benchmark, smoke-test GUI/runtime/deployed application; fix review findings and document precise support matrix.

## Verification commands
`cmake --preset macos-local`, `cmake --build --preset macos-local`, `ctest --test-dir build/macos --output-on-failure`, `ivs_data_tool --database data/benchmark.sqlite --seed 1000000 --report reports/million-records.json`, `IndustrialVisionStudio --smoke-test --workspace verification/smoke`, deployed runtime smoke test.
