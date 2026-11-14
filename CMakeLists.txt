cmake_minimum_required(VERSION 3.25)

project(Vaultik VERSION 0.11.0 LANGUAGES C CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Vaultik supports Windows and Linux")
endif()

option(VAULTIK_BUILD_GUI "Build the Qt 6 frontend (vaultik-gui)" ON)
option(VAULTIK_BUILD_TESTS "Build the tests (vaultik_tests, needs doctest)" ON)
option(VAULTIK_FUSE "Linux: browse versions as folders with FUSE 3 when libfuse3 is found" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: a single self-contained exe, which phase 5 also needs for the WinPE recovery stick.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Windows: everything from vcpkg (build.cmd). Linux: vcpkg when its toolchain is given (the
# linux-vcpkg-* presets), otherwise the distribution's packages (build.sh, the linux-* presets):
#   Debian/Ubuntu: g++ cmake ninja-build pkg-config libzstd-dev libssl-dev libcurl4-openssl-dev
#                  nlohmann-json3-dev doctest-dev qt6-base-dev libqt6svg6-dev libfuse3-dev (and
#                  libblake3-dev where the distribution has it; otherwise BLAKE3 is downloaded and
#                  built)
#   Arch/SteamOS:  base-devel cmake ninja zstd openssl curl nlohmann-json doctest qt6-base qt6-svg blake3 fuse3
if(WIN32)
    find_package(zstd CONFIG REQUIRED)
    find_package(BLAKE3 CONFIG REQUIRED)
    find_package(doctest CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
else()
    find_package(PkgConfig)
    find_package(Threads REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(CURL REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    if(VAULTIK_BUILD_TESTS)
        find_package(doctest CONFIG QUIET)
        if(NOT doctest_FOUND)
            find_path(VAULTIK_DOCTEST_DIR doctest/doctest.h PATH_SUFFIXES include REQUIRED)
            add_library(doctest::doctest INTERFACE IMPORTED)
            target_include_directories(doctest::doctest INTERFACE ${VAULTIK_DOCTEST_DIR})
        endif()
    endif()
    find_package(zstd CONFIG QUIET)
    if(NOT zstd_FOUND OR NOT (TARGET zstd::libzstd_static OR TARGET zstd::libzstd OR TARGET zstd::libzstd_shared))
        pkg_check_modules(VAULTIK_ZSTD_PC REQUIRED IMPORTED_TARGET libzstd)
        add_library(zstd::libzstd ALIAS PkgConfig::VAULTIK_ZSTD_PC)
    endif()
    find_package(BLAKE3 CONFIG QUIET)
    if(NOT BLAKE3_FOUND AND PkgConfig_FOUND)
        pkg_check_modules(VAULTIK_BLAKE3_PC QUIET IMPORTED_TARGET libblake3)
        if(VAULTIK_BLAKE3_PC_FOUND)
            add_library(BLAKE3::blake3 ALIAS PkgConfig::VAULTIK_BLAKE3_PC)
        endif()
    endif()
    if(NOT TARGET BLAKE3::blake3)
        # Not packaged by every distribution (Ubuntu 24.04 has none): the official C sources, at
        # a fixed release and hash.
        include(FetchContent)
        FetchContent_Declare(blake3
            URL https://github.com/BLAKE3-team/BLAKE3/archive/refs/tags/1.5.4.tar.gz
            URL_HASH SHA256=ddd24f26a31d23373e63d9be2e723263ac46c8b6d49902ab08024b573fd2a416
            SOURCE_SUBDIR c
            DOWNLOAD_EXTRACT_TIMESTAMP ON)
        set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_MakeAvailable(blake3)
        set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})
        set_target_properties(blake3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
endif()

if(TARGET zstd::libzstd_static)
    set(VAULTIK_ZSTD zstd::libzstd_static)
elseif(TARGET zstd::libzstd)
    set(VAULTIK_ZSTD zstd::libzstd)
else()
    set(VAULTIK_ZSTD zstd::libzstd_shared)
endif()

# ---- engine -----------------------------------------------------------------------------------

add_library(vaultik_core STATIC
    src/vaultik/util/hex.cpp
    src/vaultik/util/qr.cpp
    src/vaultik/util/ttf.cpp
    src/vaultik/util/resources.cpp
    src/vaultik/util/keep_awake.cpp
    src/vaultik/crypto/hash.cpp
    src/vaultik/crypto/md5.cpp
    src/vaultik/chunker/fastcdc.cpp
    src/vaultik/compress/zstd.cpp
    src/vaultik/model/tree.cpp
    src/vaultik/model/snapshot.cpp
    src/vaultik/repo/config.cpp
    src/vaultik/repo/pack.cpp
    src/vaultik/repo/index.cpp
    src/vaultik/repo/local_backend.cpp
    src/vaultik/repo/lock.cpp
    src/vaultik/repo/keys.cpp
    src/vaultik/repo/repository.cpp
    src/vaultik/repo/recovery.cpp
    src/vaultik/ops/backup.cpp
    src/vaultik/ops/restore.cpp
    src/vaultik/ops/restore_test.cpp
    src/vaultik/ops/rot_check.cpp
    src/vaultik/ops/history.cpp
    src/vaultik/ops/diff.cpp
    src/vaultik/ops/copy.cpp
    src/vaultik/ops/verify.cpp
    src/vaultik/ops/prune.cpp
    src/vaultik/ops/thaw.cpp
    src/vaultik/ops/repair.cpp
    src/vaultik/platform/os_common.cpp
    src/vaultik/platform/vss.cpp
    src/vaultik/platform/acl.cpp
    src/vaultik/platform/drive_health.cpp
    src/vaultik/platform/http.cpp
    src/vaultik/platform/loopback.cpp
    src/vaultik/platform/progress_channel.cpp
    src/vaultik/util/task_events.cpp
    src/vaultik/util/base64.cpp
    src/vaultik/repo/cloud.cpp
    src/vaultik/repo/oauth.cpp
    src/vaultik/repo/s3_backend.cpp
    src/vaultik/repo/pcloud_backend.cpp
    src/vaultik/repo/webdav_backend.cpp
    src/vaultik/repo/onedrive_backend.cpp
    src/vaultik/repo/gdrive_backend.cpp
    src/vaultik/repo/mega_backend.cpp
    src/vaultik/repo/backends.cpp
    src/vaultik/repo/sftp_location.cpp
    src/vaultik/jobs/job.cpp
    src/vaultik/jobs/drive_lock.cpp
    src/vaultik/jobs/check_state.cpp
    src/vaultik/jobs/protection.cpp
    src/vaultik/jobs/eject.cpp
    src/vaultik/jobs/run_log.cpp
    src/vaultik/jobs/drive_history.cpp
    src/vaultik/jobs/alerts.cpp
    src/vaultik/jobs/report.cpp
    src/vaultik/jobs/forecast.cpp
    src/vaultik/jobs/growth.cpp
    src/vaultik/jobs/recovery_kit.cpp
    src/vaultik/jobs/computers.cpp
    src/vaultik/jobs/settings_transfer.cpp
    src/vaultik/jobs/update_check.cpp
    src/vaultik/jobs/profile_items.cpp
    src/vaultik/ops/move_here.cpp
    src/vaultik/ops/apps.cpp
    src/vaultik/ops/change_journal.cpp
    src/vaultik/ops/virt.cpp
    src/vaultik/ops/docker.cpp
    src/vaultik/ops/databases.cpp
    src/vaultik/jobs/admin_jobs.cpp
    src/vaultik/jobs/scheduler.cpp
    src/vaultik/image/layout.cpp
    src/vaultik/image/extents.cpp
    src/vaultik/image/vhdx.cpp
    src/vaultik/image/ntfs.cpp
    src/vaultik/image/image.cpp
    src/vaultik/image/disk_common.cpp
    src/vaultik/image/disk_tools.cpp
    src/vaultik/image/recovery_media.cpp
)
target_include_directories(vaultik_core PUBLIC src)

# The version, from project() above: vaultik/version.hpp for the code, and on Windows a version
# resource (the file's Properties) with the program icon for each program.
configure_file(src/version.hpp.in ${CMAKE_BINARY_DIR}/generated/vaultik/version.hpp @ONLY)
target_include_directories(vaultik_core PUBLIC ${CMAKE_BINARY_DIR}/generated)

function(vaultik_version_resource target description)
    if(NOT WIN32)
        return()
    endif()
    set(VAULTIK_RC_NAME ${target})
    set(VAULTIK_RC_DESCRIPTION "${description}")
    set(VAULTIK_RC_ICON "")
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/gui/vaultik.ico)
        set(VAULTIK_RC_ICON "1 ICON \"${CMAKE_SOURCE_DIR}/src/gui/vaultik.ico\"")
    endif()
    configure_file(${CMAKE_SOURCE_DIR}/src/version.rc.in ${CMAKE_BINARY_DIR}/generated/${target}.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated/${target}.rc)
endfunction()

if(WIN32)
    target_sources(vaultik_core PRIVATE
        src/vaultik/util/win.cpp
        src/vaultik/util/resources_win.cpp
        src/vaultik/crypto/crypto_win.cpp
        src/vaultik/platform/os_win.cpp
        src/vaultik/platform/eject_win.cpp
        src/vaultik/ops/mount_win.cpp
        src/vaultik/ops/apps_win.cpp
        src/vaultik/platform/drive_health_win.cpp
        src/vaultik/platform/usn_journal_win.cpp
        src/vaultik/platform/win_extras.cpp
        src/vaultik/repo/sftp_backend.cpp
        src/vaultik/image/disk_win.cpp
    )
    find_package(libssh2 CONFIG REQUIRED)
    target_compile_definitions(vaultik_core PUBLIC
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_compile_options(vaultik_core PUBLIC
        /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor
        $<$<CONFIG:Release>:/O2 /Gy /Gw>)
    target_link_libraries(vaultik_core PUBLIC ${VAULTIK_ZSTD} BLAKE3::blake3 nlohmann_json::nlohmann_json
                          bcrypt advapi32 crypt32 ole32 vssapi winhttp ws2_32 shell32 ProjectedFSLib wbemuuid oleaut32 netapi32 virtdisk
                          setupapi cfgmgr32
                          libssh2::libssh2)
else()
    target_sources(vaultik_core PRIVATE
        src/vaultik/util/resources_posix.cpp
        src/vaultik/crypto/crypto_openssl.cpp
        src/vaultik/platform/os_posix.cpp
        src/vaultik/platform/eject_posix.cpp
        src/vaultik/ops/mount_posix.cpp
        src/vaultik/ops/apps_posix.cpp
        src/vaultik/platform/drive_health_posix.cpp
        src/vaultik/platform/usn_journal_posix.cpp
        src/vaultik/image/disk_posix.cpp
        src/vaultik/image/disk_tools_posix.cpp
    )
    target_compile_definitions(vaultik_core PUBLIC _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(vaultik_core PUBLIC -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O2>)
    target_link_libraries(vaultik_core PUBLIC ${VAULTIK_ZSTD} BLAKE3::blake3 nlohmann_json::nlohmann_json
                          OpenSSL::Crypto CURL::libcurl Threads::Threads)
    # Browsing a version as a folder (vaultik mount) uses FUSE 3 when its development files are
    # there (Debian/Ubuntu: libfuse3-dev, Fedora: fuse3-devel, Arch/SteamOS: fuse3); without them
    # everything else builds and the command says what to install.
    if(VAULTIK_FUSE AND PkgConfig_FOUND)
        pkg_check_modules(VAULTIK_FUSE3 QUIET IMPORTED_TARGET fuse3>=3.2)
    endif()
    if(VAULTIK_FUSE3_FOUND)
        target_compile_definitions(vaultik_core PRIVATE VAULTIK_HAVE_FUSE)
        target_link_libraries(vaultik_core PRIVATE PkgConfig::VAULTIK_FUSE3)
        message(STATUS "FUSE 3 ${VAULTIK_FUSE3_VERSION}: vaultik mount shows versions as folders")
    else()
        message(STATUS "FUSE 3 (libfuse3) not found: vaultik mount is not available")
    endif()
endif()

# ---- command line -----------------------------------------------------------------------------

add_executable(vaultik
    src/vaultik/cli/main.cpp
    src/vaultik/cli/console.cpp
    $<IF:$<BOOL:${WIN32}>,src/vaultik/cli/console_win.cpp,src/vaultik/cli/console_posix.cpp>
)
target_link_libraries(vaultik PRIVATE vaultik_core)
vaultik_version_resource(vaultik "Vaultik backup engine and command line")
if(WIN32)
    # The Projected File System (vaultik mount) is an optional Windows feature: load its DLL only
    # when a version is browsed, so vaultik still starts where the feature is off. The same for
    # what Windows PE (the recovery drive) may lack: shadow copies, HTTP (cloud storage), virtual
    # disks and the network account calls.
    target_link_options(vaultik PRIVATE /DELAYLOAD:ProjectedFSLib.dll /DELAYLOAD:VSSAPI.DLL /DELAYLOAD:WINHTTP.dll
                        /DELAYLOAD:VirtDisk.dll /DELAYLOAD:NETAPI32.dll)
    target_link_libraries(vaultik PRIVATE delayimp)
endif()
if(WIN32)
    target_link_options(vaultik PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)

    # Windowless launcher that Task Scheduler runs, so no console flashes up.
    add_executable(vaultik-task WIN32 src/vaultik/cli/task_launcher.cpp)
    target_compile_definitions(vaultik-task PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(vaultik-task PRIVATE /W4 /permissive- /utf-8)
    vaultik_version_resource(vaultik-task "Vaultik scheduled run launcher")
endif()
if(NOT WIN32)
    # The engine on its own too: a server without Qt runs backups from the command line and
    # systemd timers.
    install(TARGETS vaultik RUNTIME DESTINATION bin)
endif()

# ---- frontend ---------------------------------------------------------------------------------

if(VAULTIK_BUILD_GUI)
    if(WIN32)
        find_package(Qt6 6.5 COMPONENTS Widgets Svg)
    else()
        # Network: the single-instance socket (QLocalServer).
        find_package(Qt6 6.4 COMPONENTS Widgets Svg Network)
    endif()
    if(Qt6_FOUND)
        qt_add_executable(vaultik-gui WIN32
            src/gui/main.cpp
            src/gui/common.cpp
            src/gui/runner.cpp
            src/gui/repo_handle.cpp
            src/gui/job_dialog.cpp
            src/gui/restore_dialog.cpp
            src/gui/image_dialogs.cpp
            src/gui/disk_tools_dialogs.cpp
            src/gui/elevated_task.cpp
            src/gui/settings_transfer_dialog.cpp
            src/gui/apps_dialog.cpp
            src/gui/key_dialog.cpp
            src/gui/cloud_dialog.cpp
            src/gui/theme.cpp
            src/gui/job_card.cpp
            src/gui/versions_view.cpp
            src/gui/find_page.cpp
            src/gui/syntax.cpp
            src/gui/preview.cpp
            src/gui/image_decode.cpp
            src/gui/app_settings.cpp
            src/gui/settings_dialog.cpp
            src/gui/updates.cpp
            src/gui/alerts_dialog.cpp
            src/gui/report_dialog.cpp
            src/gui/grew_dialog.cpp
            src/gui/kit_print.cpp
            src/gui/tray.cpp
            src/gui/drive_watcher.cpp
            src/gui/event_runs.cpp
            src/gui/overview.cpp
            src/gui/compare_dialog.cpp
            src/gui/computers_dialog.cpp
            src/gui/hello.cpp
            src/gui/translator.cpp
            src/gui/main_window.cpp
        )
        set_target_properties(vaultik-gui PROPERTIES AUTOMOC ON)
        # Fluent UI System Icons (MIT, see src/gui/icons/LICENSE-fluentui-system-icons.txt), as :/icons/<name>.svg
        file(GLOB VAULTIK_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS src/gui/icons/*.svg)
        list(APPEND VAULTIK_ICONS src/gui/app_icon.svg)  # :/app_icon.svg
        qt_add_resources(vaultik-gui "icons" PREFIX "/" BASE src/gui FILES ${VAULTIK_ICONS})
        # Translations (English text -> translation), as :/translations/<lang>.json
        file(GLOB VAULTIK_TRANSLATIONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS translations/*.json)
        qt_add_resources(vaultik-gui "translations" PREFIX "/" FILES ${VAULTIK_TRANSLATIONS})
        target_link_libraries(vaultik-gui PRIVATE vaultik_core Qt6::Widgets Qt6::Svg)
        vaultik_version_resource(vaultik-gui "Vaultik")

        # Development tool, built on request: renders the program icon (see tools/make_icon.cpp).
        qt_add_executable(vaultik-make-icon tools/make_icon.cpp)
        set_target_properties(vaultik-make-icon PROPERTIES EXCLUDE_FROM_ALL ON)
        target_link_libraries(vaultik-make-icon PRIVATE Qt6::Gui Qt6::Svg)
        if(WIN32)
            target_link_libraries(vaultik-gui PRIVATE windowscodecs uiautomationcore windowsapp)  # previews, accessibility, Windows Hello
        else()
            target_link_libraries(vaultik-gui PRIVATE Qt6::Network)
            install(TARGETS vaultik-gui RUNTIME DESTINATION bin)
            install(FILES packaging/linux/io.github.bigisoft.Vaultik.desktop DESTINATION share/applications)
            install(FILES src/gui/app_icon.svg DESTINATION share/icons/hicolor/scalable/apps
                    RENAME io.github.bigisoft.Vaultik.svg)
            install(FILES packaging/linux/io.github.bigisoft.Vaultik.metainfo.xml DESTINATION share/metainfo)
        endif()
        # The GUI starts the engine (and the scheduler starts the launcher) from its own folder.
        add_dependencies(vaultik-gui vaultik)
        if(WIN32)
            add_dependencies(vaultik-gui vaultik-task)
        endif()
    else()
        message(STATUS "Qt 6 Widgets not found: vaultik-gui is not built")
    endif()
endif()

# ---- tests ------------------------------------------------------------------------------------

if(VAULTIK_BUILD_TESTS)
enable_testing()
add_executable(vaultik_tests
    tests/test_main.cpp
    tests/test_util.cpp
    tests/test_drive_health.cpp
    tests/test_usn_journal.cpp
    tests/test_drive_history.cpp
    tests/test_win_extras.cpp
    tests/test_resources.cpp
    tests/test_crypto.cpp
    tests/test_chunker.cpp
    tests/test_formats.cpp
    tests/test_end_to_end.cpp
    tests/test_jobs.cpp
    tests/test_checks.cpp
    tests/test_faults.cpp
    tests/test_report.cpp
    tests/test_forecast.cpp
    tests/test_kit.cpp
    tests/test_grew_rot.cpp
    tests/test_prune.cpp
    tests/test_keys.cpp
    tests/test_s3.cpp
    tests/test_archive.cpp
    tests/test_cloud_drives.cpp
    tests/test_versions.cpp
    tests/test_recovery.cpp
    tests/test_apps.cpp
    tests/test_computers.cpp
    tests/test_virt.cpp
    tests/test_containers.cpp
    tests/test_image.cpp
    tests/test_disk_tools.cpp
    tests/test_progress_channel.cpp
    tests/test_settings_transfer.cpp
    tests/test_posix.cpp
)
target_link_libraries(vaultik_tests PRIVATE vaultik_core doctest::doctest)
# Assertions run on the main thread only. Without per-thread copies of doctest's state, threads that
# Windows starts in the process (and that are still running at exit) leave nothing the debug leak
# check would report.
target_compile_definitions(vaultik_tests PRIVATE DOCTEST_CONFIG_NO_MULTITHREADING
    VAULTIK_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
    # The command-line program, for the tests of its disk tool options.
    VAULTIK_TEST_CLI="$<TARGET_FILE:vaultik>")
add_dependencies(vaultik_tests vaultik)
add_test(NAME vaultik_tests COMMAND vaultik_tests)
endif()

# The programs load the system libraries they need from System32 only, never from their own folder
# (a library planted next to them would otherwise load into an elevated run).
if(WIN32)
    foreach(program vaultik vaultik-task vaultik-gui)
        if(TARGET ${program})
            target_link_options(${program} PRIVATE /DEPENDENTLOADFLAG:0x800)
        endif()
    endforeach()
endif()
