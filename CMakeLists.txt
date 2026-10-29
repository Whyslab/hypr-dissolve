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

# Полный abi, как его считает Hyprland (PluginAPI.hpp, __hyprland_api_get_client_hash):
# commit + major.minor пяти библиотек из version.h. Сверка по одному тегу
# пропускала пересборку пакета с новыми библиотеками под тем же тегом.
set(HYPRLAND_VERSION_H "/usr/include/hyprland/src/version.h" CACHE FILEPATH "version.h Hyprland")
file(STRINGS "${HYPRLAND_VERSION_H}" _vh REGEX "^#define (GIT_COMMIT_HASH|[A-Z]+_VERSION) ")
set(DISSOLVE_BUILT_ABI "")
foreach(_key GIT_COMMIT_HASH AQUAMARINE_VERSION HYPRUTILS_VERSION HYPRGRAPHICS_VERSION HYPRCURSOR_VERSION HYPRLANG_VERSION)
    set(_val "")
    foreach(_line IN LISTS _vh)
        if(_line MATCHES "^#define ${_key} +\"([^\"]+)\"")
            set(_val "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    if(_val STREQUAL "")
        message(FATAL_ERROR "В ${HYPRLAND_VERSION_H} нет ${_key}")
    endif()
    if(_key STREQUAL "GIT_COMMIT_HASH")
        set(DISSOLVE_BUILT_ABI "${_val}")
    else()
        string(REGEX REPLACE "\\.[^.]*$" "" _mm "${_val}")   # stripPatch: до последней точки
        string(REGEX REPLACE "_VERSION$" "" _lib "${_key}")
        set(_short_AQUAMARINE aq)
        set(_short_HYPRUTILS hu)
        set(_short_HYPRGRAPHICS hg)
        set(_short_HYPRCURSOR hc)
        set(_short_HYPRLANG hlg)
        string(APPEND DISSOLVE_BUILT_ABI "_${_short_${_lib}}_${_mm}")
    endif()
endforeach()
message(STATUS "abi сборки: ${DISSOLVE_BUILT_ABI}")

target_compile_definitions(hypr-dissolve PRIVATE DISSOLVE_BUILT_ABI="${DISSOLVE_BUILT_ABI}")
# Без GNU_UNIQUE: иначе статика из inline-функций заголовков Hyprland одна на
# весь процесс (два плагина видят один client_hash), а dlclose не выгружает
# образ, и повторный dlopen того же пути отдаёт старую сборку.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(hypr-dissolve PRIVATE -fno-gnu-unique)
endif()
target_link_libraries(hypr-dissolve PRIVATE PkgConfig::deps)
# Hyprland грузит плагин через dlopen: имя без префикса lib для единообразия.
set_target_properties(hypr-dissolve PROPERTIES PREFIX "")
