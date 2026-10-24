# SCARA Robot Implementation Plan

**Goal:** A usable animated SCARA laboratory integrated with the hardware-free vision pipeline.
**Architecture:** Pure domain kinematics, one shared SimulationPlant, nonblocking pipeline gateway and a Qt Widgets console/canvas.
**Tech Stack:** C++17, Qt6 Core/Gui/Widgets/Sql/Network/Test, CMake; no new dependency.
**Spec:** docs/superpowers/specs/2026-10-24-scara-robot-design.md

## Global constraints
- All files remain in this project; real robot adapters are not implemented.
- Use existing simulated devices and interruption generation; preserve existing valid projects.
- Bounded trail240/teaching100; validate finite numeric values and atomic imports.
- Joint command speeds 1..100%, J1/J2±170°, J4±180°, Z0..150 mm.

## Review focus
- Stop/retarget must not satisfy a pending move or sort; no automatic resume after transient interlocks.
- UI refresh must preserve in-progress edits but clear them on project/device replacement.
- A held object follows the TCP; sort increments the correct bin once, only on real virtual release.
- Bad pose/teaching files do not modify robot state or silently select another device.
- Pipeline locks manual commands and makes capture/store/output plus robot_sort one completed cycle.

## Tasks
- [x] Core: RobotKinematics.h/.cpp, SimulationPlant robot state/commands and deterministic tests; RED→GREEN. Public signatures in checked-in headers are the contract.
- [x] Runtime: Devices and PipelineWorker command tokens/readiness; Project validation and FlowEditor node defaults; gateway/pipeline/domain tests RED→GREEN.
- [x] UI: RobotConsole/RobotScene with manual/joint/TCP/gripper/sort/teaching and JSON file operations; tests against real shared plant.
- [x] Integration: CMake, MainWindow navigation/add-device/state locking, dedicated example, CLI startup and launcher; integration tests.
- [x] Acceptance: build/full CTest, GUI screenshot and scenario, independent code review, updated docs and runtime package.

Run `cmake --preset macos-local`, `cmake --build --preset macos-local --parallel 4`, `ctest --test-dir build/macos --output-on-failure`.
