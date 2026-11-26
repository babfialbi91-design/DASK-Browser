cmake_minimum_required(VERSION 3.21)

# CEF_ROOT can be provided via:
#   1) -DCEF_ROOT="C:/path/to/cef_binary_..."     (CMake argument)
#   2) the CEF_ROOT environment variable
#   3) scripts/cef_root.txt (a single line with the path)
# The CEF distribution itself is NOT stored in Git (see .gitignore).
if(NOT DEFINED CEF_ROOT OR CEF_ROOT STREQUAL "")
    if(DEFINED ENV{CEF_ROOT} AND NOT "$ENV{CEF_ROOT}" STREQUAL "")
        set(CEF_ROOT "$ENV{CEF_ROOT}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cef_root.txt")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cef_root.txt" CEF_ROOT_FILE_CONTENT)
        string(STRIP "${CEF_ROOT_FILE_CONTENT}" CEF_ROOT)
    endif()
endif()

if(NOT DEFINED CEF_ROOT OR CEF_ROOT STREQUAL "" OR NOT EXISTS "${CEF_ROOT}")
    message(FATAL_ERROR
        "CEF_ROOT is not set or does not point to an existing CEF distribution.\n"
        "Run:  powershell -ExecutionPolicy Bypass -File scripts/download_cef.ps1\n"
        "Or pass:  cmake -DCEF_ROOT=\"C:/path/to/cef_binary_...\" ...")
endif()

file(TO_CMAKE_PATH "${CEF_ROOT}" CEF_ROOT)
message(STATUS "CEF_ROOT = ${CEF_ROOT}")

project(DaskBrowser LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)

# Logical target representing libcef.dll import library (Debug/Release aware).
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

set(DASK_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin")

function(dask_finalize_target target)
    set_executable_target_properties(${target})
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${DASK_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${DASK_OUTPUT_DIR}")
    # Copy CEF runtime files next to the executable.
    COPY_FILES(${target} "${CEF_BINARY_FILES}" "${CEF_BINARY_DIR}" "${DASK_OUTPUT_DIR}")
    COPY_FILES(${target} "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${DASK_OUTPUT_DIR}")
endfunction()

# Shared CEF application implementation used by both executables.
add_library(dask_cef_app STATIC
    app/native/dask_app.h
    app/native/dask_app.cpp
    app/native/dask_scheme.h
    app/native/dask_scheme.cpp)
target_include_directories(dask_cef_app PRIVATE ${CEF_ROOT} app/native)
target_link_libraries(dask_cef_app PRIVATE libcef_lib libcef_dll_wrapper Shlwapi)
dask_finalize_target(dask_cef_app)

# Main executable: browser process, owns the Win32 window.
add_executable(dask_browser WIN32
    app/native/main.cpp
    app/native/adblock/filter_engine.h
    app/native/adblock/filter_engine.cpp
    app/native/adblock/site_util.h
    app/native/filter_lists.h
    app/native/filter_lists.cpp
    app/native/privacy_settings.h
    app/native/privacy_settings.cpp
    app/native/browser_window.h
    app/native/browser_window.cpp
    app/native/tab_manager.h
    app/native/tab_manager.cpp
    app/native/dask_scheme.h
    app/native/dask_scheme.cpp)
target_include_directories(dask_browser PRIVATE ${CEF_ROOT} app/native)
target_link_libraries(dask_browser PRIVATE
    dask_cef_app
    dask_storage
    libcef_lib
    libcef_dll_wrapper
    d3d11 dxgi dxguid)
dask_finalize_target(dask_browser)

# CEF subprocess executable (renderer / gpu / utility processes).
add_executable(dask_browser_subprocess WIN32
    app/subprocess/subprocess_main.cpp)
target_include_directories(dask_browser_subprocess PRIVATE ${CEF_ROOT} app/native)
target_link_libraries(dask_browser_subprocess PRIVATE
    dask_cef_app
    libcef_lib
    libcef_dll_wrapper)
dask_finalize_target(dask_browser_subprocess)

# Storage layer: SQLite via the system winsqlite3.dll (Windows SDK lib).
add_library(dask_storage STATIC
    app/native/storage/database.h
    app/native/storage/database.cpp
    app/native/storage/stores.h
    app/native/storage/stores.cpp
    app/native/storage/download_filename.h
    app/native/storage/download_filename.cpp)
target_include_directories(dask_storage PRIVATE app/native)
target_link_libraries(dask_storage PUBLIC winsqlite3)
dask_finalize_target(dask_storage)

# Storage layer unit tests (no CEF dependency).
add_executable(dask_storage_tests
    tests/storage_test.cpp)
target_include_directories(dask_storage_tests PRIVATE app/native)
target_link_libraries(dask_storage_tests PRIVATE dask_storage)
dask_finalize_target(dask_storage_tests)

# Filter engine unit tests (no CEF dependency; run manually or via the
# smoke test).
add_executable(dask_filter_tests
    tests/filter_engine_test.cpp
    app/native/adblock/filter_engine.h
    app/native/adblock/filter_engine.cpp)
target_include_directories(dask_filter_tests PRIVATE app/native)
dask_finalize_target(dask_filter_tests)

# Copy the built React UI bundle next to the binaries (served via dask://).
add_custom_command(TARGET dask_browser POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dist"
        "$<TARGET_FILE_DIR:dask_browser>/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/test_page.html"
        "$<TARGET_FILE_DIR:dask_browser>/test_page.html"
    COMMENT "Copying UI bundle and test page")
