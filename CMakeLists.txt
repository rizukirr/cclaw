cmake_minimum_required(VERSION 3.32)
project(cclaw VERSION 0.0.1 LANGUAGES C)

# ── Options ──────────────────────────

option(CCLAW_BUILD_EXAMPLES "Build example programs"       OFF)
option(CCLAW_BUILD_TESTS    "Build tests"                  ${PROJECT_IS_TOP_LEVEL})
option(CCLAW_ASAN           "Enable ASan+UBSan in Debug"   ON)

# ── Default build type ──────────────

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ── C standard ──────────────────────

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Output dirs ─────────────────────

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(GNUInstallDirs)

# ── Version header ──────────────────

set(FULL_PROJECT_VERSION "${PROJECT_VERSION}-rc.1")
configure_file(src/version.h.in ${CMAKE_BINARY_DIR}/generated/version.h @ONLY)

# ── Dependencies ────────────────────

if(WIN32)
    include(FetchContent)
    set(CURL_DISABLE_INSTALL       ON  CACHE BOOL "" FORCE)
    set(CURL_ENABLE_EXPORT_TARGET  OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS          OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE             OFF CACHE BOOL "" FORCE)
    set(CURL_USE_SCHANNEL          ON  CACHE BOOL "" FORCE)
    FetchContent_Declare(curl
        URL https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.xz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(curl)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
endif()

# ── Compiler warnings ──────────────

add_library(cclaw_warnings INTERFACE)
if(MSVC)
    target_compile_options(cclaw_warnings INTERFACE /W4)
else()
    target_compile_options(cclaw_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wformat=2
        -Wconversion -Wsign-conversion -Wdouble-promotion
        -Wno-unused-parameter
    )
endif()

# ── Sanitizers (Debug only) ────────

add_library(cclaw_sanitizers INTERFACE)
if(CCLAW_ASAN AND NOT MSVC)
    target_compile_options(cclaw_sanitizers INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-omit-frame-pointer>)
    target_link_options(cclaw_sanitizers INTERFACE
        $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

# ── cclaw library ───────────────────

add_library(cclaw_lib STATIC
    src/core/cclaw.c
    src/core/response.c
    src/core/error.c
    src/net/http.c
    src/provider/openai.c
    src/provider/registry.c
    src/tui/tui.c
)


set_source_files_properties(src/tui/tui.c PROPERTIES COMPILE_FLAGS "-Wno-conversion -Wno-sign-conversion -Wno-shadow -Wno-double-promotion -Wno-unused-function")

target_include_directories(cclaw_lib
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(cclaw_lib
    PRIVATE cclaw_warnings cclaw_sanitizers
)

if(WIN32)
    target_link_libraries(cclaw_lib PRIVATE CURL::libcurl)
else()
    target_link_libraries(cclaw_lib PRIVATE PkgConfig::LIBCURL m)
endif()

# ── Main executable ─────────────────

add_executable(cclaw src/main.c)
target_include_directories(cclaw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(cclaw PRIVATE cclaw_lib cclaw_warnings cclaw_sanitizers)

# ── Tests ───────────────────────────

if(CCLAW_BUILD_TESTS)
    include(CTest)
    enable_testing()

    function(cclaw_add_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE cclaw_lib cclaw_warnings cclaw_sanitizers)
        target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    cclaw_add_test(test_error test/test_error.c)
    # cclaw_add_test(test_thread  test/test_thread.c)
    # cclaw_add_test(test_tool    test/test_tool.c)
    # cclaw_add_test(test_agent   test/test_agent.c)
endif()

# ── Examples ────────────────────────

if(CCLAW_BUILD_EXAMPLES)
    function(cclaw_add_example name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE cclaw_lib cclaw_warnings cclaw_sanitizers)
    endfunction()

    # cclaw_add_example(example_chat       examples/chat.c)
    # cclaw_add_example(example_streaming  examples/streaming.c)
    # cclaw_add_example(example_agent      examples/agent.c)
endif()

# ── Install ─────────────────────────

install(TARGETS cclaw_lib
    EXPORT cclawTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/cclaw/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cclaw)
install(TARGETS cclaw RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
