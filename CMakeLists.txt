cmake_minimum_required(VERSION 3.25)

project(Vaultik VERSION 0.2.0 LANGUAGES C CXX)

if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Vaultik supports Windows and Linux")
endif()

option(VAULTIK_BUILD_GUI "Build the Qt 6 frontend (vaultik-gui)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT: a single self-contained exe, which phase 5 also needs for the WinPE recovery stick.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

find_package(zstd CONFIG REQUIRED)
find_package(BLAKE3 CONFIG REQUIRED)
find_package(doctest CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
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
    src/vaultik/util/resources.cpp
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
    src/vaultik/ops/backup.cpp
    src/vaultik/ops/restore.cpp
    src/vaultik/ops/history.cpp
    src/vaultik/ops/verify.cpp
    src/vaultik/ops/prune.cpp
    src/vaultik/platform/os_common.cpp
    src/vaultik/platform/vss.cpp
    src/vaultik/platform/acl.cpp
    src/vaultik/platform/http.cpp
    src/vaultik/platform/loopback.cpp
    src/vaultik/util/base64.cpp
    src/vaultik/repo/cloud.cpp
    src/vaultik/repo/oauth.cpp
    src/vaultik/repo/s3_backend.cpp
    src/vaultik/repo/pcloud_backend.cpp
    src/vaultik/repo/onedrive_backend.cpp
    src/vaultik/repo/gdrive_backend.cpp
    src/vaultik/repo/mega_backend.cpp
    src/vaultik/repo/backends.cpp
    src/vaultik/jobs/job.cpp
    src/vaultik/jobs/admin_jobs.cpp
    src/vaultik/jobs/scheduler.cpp
)
target_include_directories(vaultik_core PUBLIC src)

if(WIN32)
    target_sources(vaultik_core PRIVATE
        src/vaultik/util/win.cpp
        src/vaultik/util/resources_win.cpp
        src/vaultik/crypto/crypto_win.cpp
        src/vaultik/platform/os_win.cpp
    )
    target_compile_definitions(vaultik_core PUBLIC
        UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
    target_compile_options(vaultik_core PUBLIC
        /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor
        $<$<CONFIG:Release>:/O2 /Gy /Gw>)
    target_link_libraries(vaultik_core PUBLIC ${VAULTIK_ZSTD} BLAKE3::blake3 nlohmann_json::nlohmann_json
                          bcrypt advapi32 crypt32 ole32 vssapi winhttp ws2_32 shell32)
else()
    target_sources(vaultik_core PRIVATE
        src/vaultik/util/resources_posix.cpp
        src/vaultik/crypto/crypto_openssl.cpp
        src/vaultik/platform/os_posix.cpp
    )
    target_compile_definitions(vaultik_core PUBLIC _GNU_SOURCE _FILE_OFFSET_BITS=64)
    target_compile_options(vaultik_core PUBLIC -Wall -Wextra -Wpedantic $<$<CONFIG:Release>:-O2>)
    target_link_libraries(vaultik_core PUBLIC ${VAULTIK_ZSTD} BLAKE3::blake3 nlohmann_json::nlohmann_json
                          OpenSSL::Crypto)
endif()

# ---- command line -----------------------------------------------------------------------------

add_executable(vaultik
    src/vaultik/cli/main.cpp
    src/vaultik/cli/console.cpp
    $<IF:$<BOOL:${WIN32}>,src/vaultik/cli/console_win.cpp,src/vaultik/cli/console_posix.cpp>
)
target_link_libraries(vaultik PRIVATE vaultik_core)
if(WIN32)
    target_link_options(vaultik PRIVATE $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)

    # Windowless launcher that Task Scheduler runs, so no console flashes up.
    add_executable(vaultik-task WIN32 src/vaultik/cli/task_launcher.cpp)
    target_compile_definitions(vaultik-task PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(vaultik-task PRIVATE /W4 /permissive- /utf-8)
endif()

# ---- frontend ---------------------------------------------------------------------------------

if(VAULTIK_BUILD_GUI)
    find_package(Qt6 6.5 COMPONENTS Widgets Svg)
    if(Qt6_FOUND)
        qt_add_executable(vaultik-gui WIN32
            src/gui/main.cpp
            src/gui/common.cpp
            src/gui/runner.cpp
            src/gui/repo_handle.cpp
            src/gui/job_dialog.cpp
            src/gui/restore_dialog.cpp
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
            src/gui/tray.cpp
            src/gui/drive_watcher.cpp
            src/gui/overview.cpp
            src/gui/main_window.cpp
        )
        set_target_properties(vaultik-gui PROPERTIES AUTOMOC ON)
        # Fluent UI System Icons (MIT, see src/gui/icons/LICENSE-fluentui-system-icons.txt), as :/icons/<name>.svg
        file(GLOB VAULTIK_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS src/gui/icons/*.svg)
        qt_add_resources(vaultik-gui "icons" PREFIX "/" BASE src/gui FILES ${VAULTIK_ICONS})
        target_link_libraries(vaultik-gui PRIVATE vaultik_core Qt6::Widgets Qt6::Svg)
        if(WIN32)
            target_link_libraries(vaultik-gui PRIVATE windowscodecs uiautomationcore)  # previews, accessibility
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

enable_testing()
add_executable(vaultik_tests
    tests/test_main.cpp
    tests/test_util.cpp
    tests/test_resources.cpp
    tests/test_crypto.cpp
    tests/test_chunker.cpp
    tests/test_formats.cpp
    tests/test_end_to_end.cpp
    tests/test_jobs.cpp
    tests/test_prune.cpp
    tests/test_keys.cpp
    tests/test_s3.cpp
    tests/test_cloud_drives.cpp
    tests/test_versions.cpp
)
target_link_libraries(vaultik_tests PRIVATE vaultik_core doctest::doctest)
add_test(NAME vaultik_tests COMMAND vaultik_tests)
