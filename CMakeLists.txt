cmake_minimum_required(VERSION 3.21)
project(skwd_paper_plasma LANGUAGES CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network OpenGL Qml Quick)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_library(skwdwallpaperplugin MODULE
    src/plugin.cpp
    src/skwdvideoitem.cpp
    src/skwdvideoitem.h
    src/skwdwindowmonitor.cpp
    src/skwdwindowmonitor.h
)

target_link_libraries(skwdwallpaperplugin PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::OpenGL Qt6::Qml Qt6::Quick EGL PkgConfig::LIBDRM)

set(SKWD_QML_DIR "${CMAKE_INSTALL_LIBDIR}/qt6/qml/org/skwd/wallpaper")
set(SKWD_LICENSE_DIR "${CMAKE_INSTALL_DATADIR}/licenses/skwd-paper-plasma")
install(TARGETS skwdwallpaperplugin LIBRARY DESTINATION "${SKWD_QML_DIR}")
install(FILES qmldir DESTINATION "${SKWD_QML_DIR}")
install(DIRECTORY wallpaper/ DESTINATION "${CMAKE_INSTALL_DATADIR}/plasma/wallpapers/org.skwd.wall.plasma")
install(FILES LICENSE DESTINATION "${SKWD_LICENSE_DIR}")

include(CTest)
if(BUILD_TESTING)
    add_executable(window-state-test scripts/tests/window-state.cpp src/skwdwindowmonitor.cpp src/skwdwindowmonitor.h)
    target_include_directories(window-state-test PRIVATE src)
    target_link_libraries(window-state-test PRIVATE Qt6::Core Qt6::Network)
    add_test(NAME window-state COMMAND window-state-test)
    set_tests_properties(window-state PROPERTIES TIMEOUT 20)
    add_executable(presentation-test scripts/tests/presentation.cpp src/skwdvideoitem.cpp src/skwdvideoitem.h)
    target_include_directories(presentation-test PRIVATE src)
    target_link_libraries(presentation-test PRIVATE Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Qml Qt6::Quick EGL PkgConfig::LIBDRM)
    add_test(NAME presentation COMMAND presentation-test)
    set_tests_properties(presentation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 25)
    add_executable(gpu-presentation-test scripts/tests/gpu-presentation.cpp src/skwdvideoitem.cpp src/skwdvideoitem.h)
    target_include_directories(gpu-presentation-test PRIVATE src)
    target_link_libraries(gpu-presentation-test PRIVATE Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Qml Qt6::Quick EGL PkgConfig::LIBDRM)
endif()
