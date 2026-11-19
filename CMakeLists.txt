cmake_minimum_required(VERSION 3.16)
project(overlay-timer
    VERSION 0.1.0
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)  # we need getopt_long / POSIX

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wshadow)
add_definitions(-D_GNU_SOURCE)

# ------------------------------------------------------------------
# Build options: choice of window backends
# ------------------------------------------------------------------
# On macOS / Windows only the native backend is built by default.
if(APPLE OR WIN32)
    set(_unix_backends_default OFF)
else()
    set(_unix_backends_default ON)
endif()
option(WITH_X11 "Build the X11 (XCB) backend" ${_unix_backends_default})
option(WITH_WAYLAND "Build the Wayland (wlr-layer-shell) backend" ${_unix_backends_default})
option(WITH_MACOS "Build the macOS (AppKit) backend" ${APPLE})
option(WITH_WIN32 "Build the Windows (Win32) backend" ${WIN32})

if(NOT WITH_X11 AND NOT WITH_WAYLAND AND NOT WITH_MACOS AND NOT WITH_WIN32)
    message(FATAL_ERROR
        "At least one of WITH_X11, WITH_WAYLAND, WITH_MACOS or WITH_WIN32 must be ON")
endif()

# ------------------------------------------------------------------
# Installation / paths
# ------------------------------------------------------------------
include(GNUInstallDirs)
set(LOCALEDIR "${CMAKE_INSTALL_FULL_LOCALEDIR}")

# ------------------------------------------------------------------
# Core (backend-independent)
# ------------------------------------------------------------------
set(CORE_SOURCES
    src/util.c
    src/config.c
    src/cli.c
    src/ipc.c
    src/daemon.c
    src/backend.c
    src/render.c
    src/timer.c
    src/audio.c
)

# Windows has its own IPC (Winsock AF_UNIX, named mutex, self-relaunch).
if(WIN32)
    list(REMOVE_ITEM CORE_SOURCES src/ipc.c)
    list(APPEND CORE_SOURCES src/ipc_win32.c)
endif()

add_library(overlay_timer_core STATIC ${CORE_SOURCES})
target_include_directories(overlay_timer_core PUBLIC src)
target_compile_definitions(overlay_timer_core PUBLIC
    OVERLAY_TIMER_VERSION="${PROJECT_VERSION}"
    OVERLAY_TIMER_LOCALEDIR="${LOCALEDIR}"
)

# ------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
# IMPORTED_TARGET carries full library paths, needed for non-default
# prefixes such as Homebrew's /opt/homebrew on macOS.
pkg_check_modules(CAIRO REQUIRED IMPORTED_TARGET cairo)
pkg_check_modules(PANGO REQUIRED IMPORTED_TARGET pangocairo)
pkg_check_modules(AO REQUIRED IMPORTED_TARGET ao)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)

find_package(Threads REQUIRED)
# gettext: part of glibc on Linux, separate libintl on macOS (Homebrew).
find_package(Intl REQUIRED)

target_include_directories(overlay_timer_core PUBLIC ${Intl_INCLUDE_DIRS})
target_link_libraries(overlay_timer_core PUBLIC
    PkgConfig::CAIRO PkgConfig::PANGO PkgConfig::AO PkgConfig::SNDFILE
    ${Intl_LIBRARIES} Threads::Threads m)

if(WITH_X11)
    pkg_check_modules(XCB REQUIRED xcb xcb-shape xcb-randr)
    target_sources(overlay_timer_core PRIVATE src/backend_x11.c)
    target_compile_definitions(overlay_timer_core PUBLIC WITH_X11)
    target_include_directories(overlay_timer_core PUBLIC ${XCB_INCLUDE_DIRS})
    target_link_libraries(overlay_timer_core PUBLIC ${XCB_LIBRARIES})
endif()

if(WITH_WAYLAND)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

    set(GEN_DIR "${CMAKE_BINARY_DIR}/wl-protocols")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(LS_XML "${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml")
    set(XDG_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")

    # wlr-layer-shell
    add_custom_command(
        OUTPUT "${GEN_DIR}/wlr-layer-shell-client-protocol.h"
        COMMAND ${WAYLAND_SCANNER} client-header "${LS_XML}"
                "${GEN_DIR}/wlr-layer-shell-client-protocol.h"
        DEPENDS "${LS_XML}" VERBATIM)
    add_custom_command(
        OUTPUT "${GEN_DIR}/wlr-layer-shell-protocol.c"
        COMMAND ${WAYLAND_SCANNER} private-code "${LS_XML}"
                "${GEN_DIR}/wlr-layer-shell-protocol.c"
        DEPENDS "${LS_XML}" VERBATIM)

    # xdg-shell (needed to resolve xdg_popup_interface referenced by the
    # layer-shell, even though we do not use popups)
    add_custom_command(
        OUTPUT "${GEN_DIR}/xdg-shell-client-protocol.h"
        COMMAND ${WAYLAND_SCANNER} client-header "${XDG_XML}"
                "${GEN_DIR}/xdg-shell-client-protocol.h"
        DEPENDS "${XDG_XML}" VERBATIM)
    add_custom_command(
        OUTPUT "${GEN_DIR}/xdg-shell-protocol.c"
        COMMAND ${WAYLAND_SCANNER} private-code "${XDG_XML}"
                "${GEN_DIR}/xdg-shell-protocol.c"
        DEPENDS "${XDG_XML}" VERBATIM)

    target_sources(overlay_timer_core PRIVATE
        src/backend_wayland.c
        "${GEN_DIR}/wlr-layer-shell-protocol.c"
        "${GEN_DIR}/wlr-layer-shell-client-protocol.h"
        "${GEN_DIR}/xdg-shell-protocol.c"
        "${GEN_DIR}/xdg-shell-client-protocol.h")
    target_compile_definitions(overlay_timer_core PUBLIC WITH_WAYLAND)
    target_include_directories(overlay_timer_core PUBLIC
        ${WAYLAND_INCLUDE_DIRS} "${GEN_DIR}")
    target_link_libraries(overlay_timer_core PUBLIC ${WAYLAND_LIBRARIES})
endif()

if(WITH_MACOS)
    if(NOT APPLE)
        message(FATAL_ERROR "WITH_MACOS requires building on macOS")
    endif()
    enable_language(OBJC)
    # Manual retain/release (no ARC): NSWindow pointers live in C structs.
    target_sources(overlay_timer_core PRIVATE src/backend_macos.m)
    target_compile_definitions(overlay_timer_core PUBLIC WITH_MACOS)
    target_link_libraries(overlay_timer_core PUBLIC
        "-framework AppKit" "-framework QuartzCore")
endif()

if(WITH_WIN32)
    if(NOT WIN32)
        message(FATAL_ERROR "WITH_WIN32 requires building on Windows")
    endif()
    pkg_check_modules(CAIRO_WIN32 REQUIRED IMPORTED_TARGET cairo-win32)
    target_sources(overlay_timer_core PRIVATE src/backend_win32.c)
    target_compile_definitions(overlay_timer_core PUBLIC WITH_WIN32)
    target_link_libraries(overlay_timer_core PUBLIC
        PkgConfig::CAIRO_WIN32 user32 gdi32)
endif()

if(WIN32)
    # Windows 10 APIs (AF_UNIX, WSAPoll); Winsock for the IPC.
    target_compile_definitions(overlay_timer_core PUBLIC _WIN32_WINNT=0x0A00)
    target_link_libraries(overlay_timer_core PUBLIC ws2_32)
endif()

# ------------------------------------------------------------------
# Executable
# ------------------------------------------------------------------
add_executable(overlay-timer src/main.c)
target_link_libraries(overlay-timer PRIVATE overlay_timer_core)

install(TARGETS overlay-timer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES man/overlay-timer.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1 OPTIONAL)

# ------------------------------------------------------------------
# i18n (gettext)
# ------------------------------------------------------------------
find_program(MSGFMT_EXECUTABLE msgfmt)
if(MSGFMT_EXECUTABLE)
    set(PO_LANGS pt_BR en)
    set(MO_FILES "")
    foreach(lang ${PO_LANGS})
        set(po "${CMAKE_SOURCE_DIR}/po/${lang}.po")
        set(mo "${CMAKE_BINARY_DIR}/po/${lang}/overlay-timer.mo")
        add_custom_command(
            OUTPUT "${mo}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_BINARY_DIR}/po/${lang}"
            COMMAND ${MSGFMT_EXECUTABLE} "${po}" -o "${mo}"
            DEPENDS "${po}" VERBATIM)
        list(APPEND MO_FILES "${mo}")
        install(FILES "${mo}"
                DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${lang}/LC_MESSAGES")
    endforeach()
    add_custom_target(translations ALL DEPENDS ${MO_FILES})
else()
    message(WARNING "msgfmt not found: translations will not be built")
endif()

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
enable_testing()
add_executable(test_util tests/test_util.c)
target_link_libraries(test_util PRIVATE overlay_timer_core)
add_test(NAME util COMMAND test_util)

add_executable(test_config tests/test_config.c)
target_link_libraries(test_config PRIVATE overlay_timer_core)
add_test(NAME config COMMAND test_config)

add_executable(test_timer tests/test_timer.c)
target_link_libraries(test_timer PRIVATE overlay_timer_core)
add_test(NAME timer COMMAND test_timer)

add_executable(test_cli tests/test_cli.c)
target_link_libraries(test_cli PRIVATE overlay_timer_core)
add_test(NAME cli COMMAND test_cli)

add_executable(test_config_kv tests/test_config_kv.c)
target_link_libraries(test_config_kv PRIVATE overlay_timer_core)
add_test(NAME config_kv COMMAND test_config_kv)