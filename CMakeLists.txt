cmake_minimum_required(VERSION 3.27)

project(hypr-dissolve
    DESCRIPTION "Telegram-style pixel dissolve on window close for Hyprland"
    VERSION 0.1.0
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    pixman-1
    libdrm
    libinput
    wayland-server
    xkbcommon
    hyprutils
    hyprlang
    hyprgraphics
    aquamarine
    hyprcursor
    glesv2
    egl)

# Версия Hyprland, под заголовки которой собираемся. Плагин намертво привязан к
# ABI конкретной сборки, поэтому в рантайме сверяемся с ней и отказываемся
# грузиться при расхождении — падение плагина роняет всю сессию.
pkg_get_variable(HYPRLAND_PC_VERSION hyprland version)
if(NOT HYPRLAND_PC_VERSION)
    execute_process(COMMAND pkg-config --modversion hyprland
                    OUTPUT_VARIABLE HYPRLAND_PC_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
message(STATUS "Сборка под Hyprland ${HYPRLAND_PC_VERSION}")

add_library(hypr-dissolve SHARED
    src/main.cpp
    src/DissolvePass.cpp)

target_compile_definitions(hypr-dissolve PRIVATE DISSOLVE_BUILT_FOR="${HYPRLAND_PC_VERSION}")
target_link_libraries(hypr-dissolve PRIVATE PkgConfig::deps)
# Hyprland грузит плагин через dlopen: имя без префикса lib для единообразия.
set_target_properties(hypr-dissolve PROPERTIES PREFIX "")
