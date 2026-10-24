cmake_minimum_required(VERSION 3.21)
project(IndustrialVisionStudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

option(IVS_BUILD_APP "Build the desktop studio and data tool" ON)
option(IVS_BUILD_MODULE_TESTS "Build storage/device/UI integration tests" ON)
option(IVS_WITH_OPENCV "Use installed OpenCV for image analysis" OFF)
option(IVS_WITH_HALCON "Link an installed licensed HALCON SDK" OFF)
option(IVS_WITH_MVS "Discover and link MVS SDK extension hook (capture adapter not implemented)" OFF)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Sql Network Concurrent)
include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()

add_library(ivs_domain STATIC
    src/domain/Types.h src/domain/Project.h src/domain/Project.cpp
    src/domain/RobotKinematics.h src/domain/RobotKinematics.cpp
    src/domain/ImageAnalysis.h src/domain/ImageAnalysis.cpp)
target_include_directories(ivs_domain PUBLIC src)
target_link_libraries(ivs_domain PUBLIC Qt6::Core Qt6::Gui)
if(IVS_WITH_OPENCV)
    find_package(OpenCV 4 REQUIRED COMPONENTS core imgproc)
    target_compile_definitions(ivs_domain PRIVATE IVS_WITH_OPENCV)
    target_include_directories(ivs_domain PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(ivs_domain PRIVATE ${OpenCV_LIBS})
endif()
if(IVS_WITH_HALCON)
    set(HALCON_ROOT "$ENV{HALCONROOT}" CACHE PATH "HALCON installation")
    set(HALCON_ARCH "$ENV{HALCONARCH}" CACHE STRING "HALCON target architecture")
    find_path(HALCON_INCLUDE_DIR HalconCpp.h HINTS "${HALCON_ROOT}/include/halconcpp" REQUIRED)
    find_library(HALCON_CPP_LIBRARY NAMES halconcpp HINTS "${HALCON_ROOT}/lib/${HALCON_ARCH}" REQUIRED)
    find_library(HALCON_LIBRARY NAMES halcon HINTS "${HALCON_ROOT}/lib/${HALCON_ARCH}" REQUIRED)
    target_include_directories(ivs_domain PRIVATE ${HALCON_INCLUDE_DIR})
    target_link_libraries(ivs_domain PRIVATE ${HALCON_CPP_LIBRARY} ${HALCON_LIBRARY})
    target_compile_definitions(ivs_domain PRIVATE IVS_WITH_HALCON)
endif()

if(IVS_BUILD_APP OR (BUILD_TESTING AND IVS_BUILD_MODULE_TESTS))
    add_library(ivs_infrastructure STATIC
        src/infrastructure/RecordRepository.h src/infrastructure/RecordRepository.cpp
        src/infrastructure/Devices.h src/infrastructure/Devices.cpp
        src/infrastructure/SimulationPlant.h src/infrastructure/SimulationPlant.cpp
        src/infrastructure/HikMvsCamera.h src/infrastructure/HikMvsCamera.cpp
        src/infrastructure/ModbusTcpClient.h src/infrastructure/ModbusTcpClient.cpp)
    target_link_libraries(ivs_infrastructure PUBLIC ivs_domain Qt6::Sql Qt6::Network)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/HardwareSdk.cmake")
        include(cmake/HardwareSdk.cmake)
        ivs_configure_hardware_sdk(ivs_infrastructure)
    endif()
    add_library(ivs_widgets STATIC
        src/ui/FlowEditor.h src/ui/FlowEditor.cpp
        src/ui/PanelEditor.h src/ui/PanelEditor.cpp
        src/ui/ChartWidget.h src/ui/ChartWidget.cpp
        src/ui/ImageView.h src/ui/ImageView.cpp
        src/ui/SimulationConsole.h src/ui/SimulationConsole.cpp
        src/ui/RobotConsole.h src/ui/RobotConsole.cpp
        src/ui/RobotScene.h src/ui/RobotScene.cpp)
    target_link_libraries(ivs_widgets PUBLIC ivs_infrastructure Qt6::Widgets)
endif()

if(IVS_BUILD_APP)
    add_library(ivs_application STATIC
        src/application/PipelineWorker.h src/application/PipelineWorker.cpp
        src/application/Publisher.h src/application/Publisher.cpp)
    target_link_libraries(ivs_application PUBLIC ivs_infrastructure Qt6::Widgets)
    target_compile_definitions(ivs_application PRIVATE
        IVS_QT_BIN_DIR="${Qt6_DIR}/../../../bin")
    qt_add_executable(IndustrialVisionStudio WIN32 MACOSX_BUNDLE
        src/main.cpp src/ui/MainWindow.h src/ui/MainWindow.cpp
        src/ui/RecordTableModel.h src/ui/RecordTableModel.cpp)
    target_link_libraries(IndustrialVisionStudio PRIVATE ivs_application ivs_widgets Qt6::Concurrent)
    target_compile_definitions(IndustrialVisionStudio PRIVATE IVS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    set_target_properties(IndustrialVisionStudio PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Industrial Vision Studio"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.industrialvision.studio"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

    add_executable(ivs_data_tool tools/data_tool.cpp)
    target_link_libraries(ivs_data_tool PRIVATE ivs_infrastructure)
    install(TARGETS IndustrialVisionStudio ivs_data_tool
        BUNDLE DESTINATION . RUNTIME DESTINATION .)
    install(DIRECTORY examples DESTINATION .)
    install(FILES README.md docs/hardware.md docs/storage.md docs/runtime.md docs/publishing.md docs/simulation-console.md docs/robot-simulation.md DESTINATION docs)
    qt_generate_deploy_app_script(TARGET IndustrialVisionStudio
        OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
    install(SCRIPT ${deploy_script})
    set(CPACK_PACKAGE_NAME "IndustrialVisionStudio")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_GENERATOR "ZIP")
    include(CPack)
endif()

if(BUILD_TESTING)
    add_executable(tst_domain tests/tst_domain.cpp)
    target_link_libraries(tst_domain PRIVATE ivs_domain Qt6::Test)
    add_test(NAME domain COMMAND tst_domain)
    if(IVS_BUILD_MODULE_TESTS)
        add_executable(tst_repository tests/tst_repository.cpp)
        target_link_libraries(tst_repository PRIVATE ivs_infrastructure Qt6::Test)
        add_test(NAME repository COMMAND tst_repository)
        add_executable(tst_devices tests/tst_devices.cpp)
        target_link_libraries(tst_devices PRIVATE ivs_infrastructure Qt6::Test)
        add_test(NAME devices COMMAND tst_devices)
        add_executable(tst_simulation tests/tst_simulation.cpp)
        target_link_libraries(tst_simulation PRIVATE ivs_infrastructure Qt6::Test)
        add_test(NAME simulation COMMAND tst_simulation)
        add_executable(tst_robot tests/tst_robot.cpp)
        target_link_libraries(tst_robot PRIVATE ivs_infrastructure Qt6::Test)
        add_test(NAME robot COMMAND tst_robot)
        add_executable(tst_robot_console tests/tst_robot_console.cpp)
        target_link_libraries(tst_robot_console PRIVATE ivs_widgets Qt6::Test)
        add_test(NAME robot_console COMMAND tst_robot_console)
        set_tests_properties(robot_console PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(tst_simulation_console tests/tst_simulation_console.cpp)
        target_link_libraries(tst_simulation_console PRIVATE ivs_widgets Qt6::Test)
        add_test(NAME simulation_console COMMAND tst_simulation_console)
        set_tests_properties(simulation_console PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(tst_editors tests/tst_editors.cpp)
        target_link_libraries(tst_editors PRIVATE ivs_widgets Qt6::Test)
        add_test(NAME editors COMMAND tst_editors)
        set_tests_properties(editors PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        if(IVS_BUILD_APP)
            add_executable(tst_pipeline tests/tst_pipeline.cpp)
            target_link_libraries(tst_pipeline PRIVATE ivs_application Qt6::Test)
            add_test(NAME pipeline COMMAND tst_pipeline)
            add_executable(tst_publisher tests/tst_publisher.cpp)
            target_link_libraries(tst_publisher PRIVATE ivs_application Qt6::Test)
            add_test(NAME publisher COMMAND tst_publisher)
            add_executable(tst_mainwindow tests/tst_mainwindow.cpp src/ui/MainWindow.h src/ui/MainWindow.cpp src/ui/RecordTableModel.h src/ui/RecordTableModel.cpp)
            target_link_libraries(tst_mainwindow PRIVATE ivs_application ivs_widgets Qt6::Concurrent Qt6::Test)
            add_test(NAME mainwindow COMMAND tst_mainwindow)
            set_tests_properties(mainwindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endif()
    endif()
endif()
