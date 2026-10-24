# Simulation Console Implementation Plan

**Goal:** Complete stateful virtual motion, lighting, PLC/I/O and a visual console in the existing Qt project.
**Spec:** docs/simulation-console-design.md
**Architecture:** One thread-safe shared SimulationPlant; the console manipulates it and DeviceGateway/PipelineWorker execute against it. Keep simulated and real hardware branches separate.

## Global constraints
- All code/data/docs remain in this project directory; no vendor SDK or external service required.
- Existing valid projects and default pixel measurements continue to work.
- Actual state mutations and trigger conditions, not no-op success responses.
- Worker owns runtime I/O; UI controls only thread-safe simulation state.

## Tasks
- [x] Stateful plant: deterministic clock, per-device axes/lights/registers, image rendering, modes and faults; tests RED→GREEN.
- [x] Gateway/runtime: shared plant injection, real simulated node commands, polling movement/trigger, output writes and cancellation; tests RED→GREEN.
- [x] Console widget: live axis controls, channel lighting, PLC/I/O tables, trigger/mode/fault controls and tests.
- [x] MainWindow/CMake integration, runtime-accessible navigation, example configuration and startup entry.
- [x] Full regression, UI interaction/screenshot, pipeline scenarios, independent review and updated runtime package/docs.

## Acceptance (2026-10-24)
- Root `macos-local` build succeeded; all 9 CTest suites passed (33.78 s).
- Real GUI run and relocated deployed runtime each committed 6 SQLite records with 6 retained images; first five results were 20 mm OK ×4 and 23 mm NG.
- New runtime package at `publish/VisionRuntime-SimulationConsole-macOS` has an empty data directory, passes codesign verification, and 12 Mach-O binaries have no developer-home library dependencies.
- Independent review fixes: latch transient interruptions until restart; discard drafts on project/address changes; disconnect focused-editor callbacks before UI teardown.
- Evidence: `reports/simulation-console-tests.xml`, `reports/simulation-console-verification.json`, and console screenshots.

## Review focus
- Invalid route, axis/channel/table/address must not reach a different device.
- UI state must survive startup but not leak between different project devices.
- Signal gating must not be bypassed by automatic trigger mode when requireValue is explicit.
- A stopped/cancelled pipeline must not leave axes moving or commit delayed results.
- Console controls cannot operate a real device backend, even with matching IDs.
