# Set CMake minimum version
cmake_minimum_required(VERSION 3.15)

# Set project name
project(uOS VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel" FORCE)
    endif()
    add_compile_options(-Wall -Wextra)
endif()
message(STATUS "Current build type: ${CMAKE_BUILD_TYPE}")

# TUI and CLI are opt-in: off by default so a regular build does not pull their deps.
option(uOS_ENABLE_TUI "Build with TUI support (finalcut, gpm, ncurses)" OFF)
option(uOS_ENABLE_CLI "Build with CLI support (cli, nlohmann/json, asio)" OFF)
option(uOS_BUILD_TESTS "Build the framework test executables" OFF)
if(uOS_ENABLE_TUI)
    message(STATUS "TUI support: ENABLED")
else()
    message(STATUS "TUI support: DISABLED (enable with -DuOS_ENABLE_TUI=ON)")
endif()
if(uOS_ENABLE_CLI)
    message(STATUS "CLI support: ENABLED")
else()
    message(STATUS "CLI support: DISABLED (enable with -DuOS_ENABLE_CLI=ON)")
endif()
if(uOS_BUILD_TESTS)
    message(STATUS "Framework tests: ENABLED")
else()
    message(STATUS "Framework tests: DISABLED (enable with -DuOS_BUILD_TESTS=ON)")
endif()

# Set target
set(TARGET uOS)

# Define directories
set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")

# Gather source files
set(SOURCES
    ${SRC_DIR}/uOS.cpp
    ${SRC_DIR}/state_machine.cpp
    ${SRC_DIR}/task.cpp
    ${SRC_DIR}/async_task.cpp
    ${SRC_DIR}/sync_task.cpp
    ${SRC_DIR}/logger.cpp)
if(uOS_ENABLE_TUI)
    list(APPEND SOURCES ${SRC_DIR}/flogview.cpp)
endif()

# Create library
add_library(${PROJECT_NAME} STATIC ${SOURCES})

# Include the FetchContent module
include(FetchContent)

############ ASIO / CLI / JSON (optional CLI) ############
if(uOS_ENABLE_CLI)
    # Check for ASIO header
    find_path(ASIO_INCLUDE_DIR NAMES asio.hpp
              PATH_SUFFIXES asio
              PATHS
                /usr/include
                /usr/local/include
                ${CMAKE_INCLUDE_PATH})

    if(ASIO_INCLUDE_DIR)
        message(STATUS "ASIO found: ${ASIO_INCLUDE_DIR}")
    else()
        message(FATAL_ERROR "ASIO not found. Please install ASIO or specify its location.")
    endif()

    find_path(CLI_INCLUDE_DIR
        NAMES cli/cli.h
        PATHS /usr/include /usr/local/include
        PATH_SUFFIXES cli)

    if(CLI_INCLUDE_DIR)
        message(STATUS "CLI found at ${CLI_INCLUDE_DIR}")
    else()
        message(STATUS "CLI not found. Fetching from source.")
        FetchContent_Declare(cli
            GIT_REPOSITORY https://github.com/daniele77/cli.git
            GIT_TAG v2.2.0
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(cli)

        # extract cli::cli's include dir for consistency
        get_target_property(CLI_INCLUDE_DIR cli::cli INTERFACE_INCLUDE_DIRECTORIES)
    endif()

    find_package(nlohmann_json 3.12.0 QUIET)
    if(nlohmann_json_FOUND)
        message(STATUS "nlohmann/json found via find_package")
    else()
        message(STATUS "nlohmann/json not found. Fetching from source.")
        FetchContent_Declare(json
            URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        set(JSON_Install ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(json)
    endif()
    get_target_property(JSON_INCLUDE_DIR nlohmann_json::nlohmann_json INTERFACE_INCLUDE_DIRECTORIES)
endif()

############ QUILL ############
set(QUILL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(QUILL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QUILL_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(quill
    GIT_REPOSITORY https://github.com/odygrd/quill.git
    GIT_TAG v13.0.0
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(QUILL_DISABLE_NON_PREFIXED_MACROS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(quill)

############ FINALCUT (optional TUI) ############
if(uOS_ENABLE_TUI)
    # First, try to find an existing installation of finalcut in system paths
    find_path(FINALCUT_INCLUDE_DIR NAMES final.h
              PATH_SUFFIXES final
              PATHS /usr/include /usr/local/include ${LIB_DIR}/finalcut/include)

    find_library(FINALCUT_LIBRARY NAMES final
                 PATHS /usr/lib /usr/local/lib ${LIB_DIR}/finalcut/lib)

    # This variable will track if we found finalcut
    set(FINALCUT_FOUND FALSE)
    if(FINALCUT_INCLUDE_DIR AND FINALCUT_LIBRARY)
        set(FINALCUT_FOUND TRUE)
        # There is an excess final in include path so get the parent directory
        get_filename_component(FINALCUT_INCLUDE_DIR "${FINALCUT_INCLUDE_DIR}" DIRECTORY)
        message(STATUS "Found pre-installed finalcut: ${FINALCUT_INCLUDE_DIR} and ${FINALCUT_LIBRARY}")
    endif()

    # If finalcut was NOT found on the system, download and build it
    if(NOT FINALCUT_FOUND)
        message(STATUS "Finalcut not found. Preparing to build from source.")
        include(ExternalProject)

        # --- Dependencies for building finalcut ---
        find_library(GPM_LIB gpm REQUIRED)
        find_library(NCURSES_LIB ncurses REQUIRED)
        find_library(RT_LIB rt REQUIRED)

        # --- Define installation directory within the build tree ---
        set(FINALCUT_INSTALL_DIR "${LIB_DIR}/finalcut")
        set(FINALCUT_INCLUDE_DIR "${FINALCUT_INSTALL_DIR}/include")
        set(FINALCUT_LIBRARY     "${FINALCUT_INSTALL_DIR}/lib/libfinal.a")

        # --- Define the external project ---
        ExternalProject_Add(finalcut_repo # Renamed to avoid conflicts
            GIT_REPOSITORY      https://github.com/gansm/finalcut.git
            GIT_TAG             main
            UPDATE_COMMAND      ""      # CRITICAL: Prevent CMake from checking for updates on every build
            PREFIX              ${CMAKE_BINARY_DIR}/_deps/finalcut
            CONFIGURE_COMMAND   cd <SOURCE_DIR> && autoreconf --install && ./configure --prefix=${FINALCUT_INSTALL_DIR} --disable-shared --enable-static
            BUILD_COMMAND       cd <SOURCE_DIR> && make -C final -j$(nproc)
            INSTALL_COMMAND     cd <SOURCE_DIR> && make -C final install && make install-pkgconfigDATA
            BUILD_BYPRODUCTS    ${FINALCUT_LIBRARY}
        )

        # Add a dependency to ensure your library waits for finalcut to be built
        add_dependencies(${PROJECT_NAME} finalcut_repo)
        message(STATUS "Finalcut will be built and installed to: ${FINALCUT_INSTALL_DIR}")

        # Install finalcut files to the system prefix during 'make install'
        install(DIRECTORY ${FINALCUT_INSTALL_DIR}/include/final/ DESTINATION include/final)
        install(FILES ${FINALCUT_LIBRARY} DESTINATION lib)
        install(DIRECTORY ${FINALCUT_INSTALL_DIR}/lib/pkgconfig/ DESTINATION lib/pkgconfig)
        install(DIRECTORY ${FINALCUT_INSTALL_DIR}/share/fonts/ DESTINATION share/fonts)

        # set difference for build and install
        set(FINALCUT_LINK $<BUILD_INTERFACE:${FINALCUT_LIBRARY}> $<INSTALL_INTERFACE:final>)
    else()
        set(FINALCUT_LINK ${FINALCUT_LIBRARY})
    endif()
endif()

############ BACKWARD ############
# Use backward-cpp's CMake so BACKWARD_HAS_* is set and the right debug-info
# library is linked. Auto-detect order is libdw → libbfd → libdwarf →
# backtrace_symbols. On Linux we reject the last of those: a DWARF reader is
# required, and the error tells the user to install libdw. Darwin only supports
# backtrace_symbols and does not need extra packages.
set(BACKWARD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(backward
    GIT_REPOSITORY https://github.com/bombela/backward-cpp
    GIT_TAG        master
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
# backward's CMakeLists installs its header even as a subproject; keep that
# out of `cmake --install` for uOS.
set(_uos_skip_install "${CMAKE_SKIP_INSTALL_RULES}")
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_MakeAvailable(backward)
set(CMAKE_SKIP_INSTALL_RULES "${_uos_skip_install}")

get_target_property(_uos_bw_defs Backward::Interface INTERFACE_COMPILE_DEFINITIONS)
if(NOT _uos_bw_defs)
    set(_uos_bw_defs "${BACKWARD_DEFINITIONS}")
endif()
if("BACKWARD_HAS_DW=1" IN_LIST _uos_bw_defs)
    set(_uos_bw_backend dw)
    message(STATUS "backward-cpp debug info: libdw")
elseif("BACKWARD_HAS_BFD=1" IN_LIST _uos_bw_defs)
    set(_uos_bw_backend bfd)
    message(STATUS "backward-cpp debug info: libbfd")
elseif("BACKWARD_HAS_DWARF=1" IN_LIST _uos_bw_defs)
    set(_uos_bw_backend dwarf)
    message(STATUS "backward-cpp debug info: libdwarf")
else()
    set(_uos_bw_backend symbols)
    message(STATUS "backward-cpp debug info: backtrace_symbols")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND _uos_bw_backend STREQUAL "symbols")
    message(FATAL_ERROR
        "No DWARF debug-info library found for backward-cpp "
        "(tried libdw, libbfd, libdwarf). Install libdw (e.g. libdw-dev / elfutils).")
endif()

# Include directories
if(uOS_ENABLE_TUI)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FINALCUT_INCLUDE_DIR})
    target_compile_definitions(${PROJECT_NAME} PUBLIC uOS_ENABLE_TUI)
endif()
if(uOS_ENABLE_CLI)
    target_include_directories(${PROJECT_NAME} PRIVATE ${ASIO_INCLUDE_DIR})
    target_include_directories(${PROJECT_NAME} PRIVATE ${CLI_INCLUDE_DIR})
    target_include_directories(${PROJECT_NAME} PRIVATE ${JSON_INCLUDE_DIR})
    target_compile_definitions(${PROJECT_NAME} PUBLIC uOS_ENABLE_CLI)
endif()
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include/uOS>
    $<INSTALL_INTERFACE:include>)
target_compile_definitions(${PROJECT_NAME} PUBLIC QUILL_DISABLE_NON_PREFIXED_MACROS)

# link libraries
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)
# Build tree only. Exporting quill::quill would make the installed package require Quill.
# Installed consumers get the vendored headers under include/uOS/quill.
target_link_libraries(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:quill::quill>)
# BUILD_INTERFACE: uOS is static, so a PRIVATE dep would otherwise be
# exported and cmake --install would demand an exported backward_interface.
target_link_libraries(${PROJECT_NAME} PRIVATE
    $<BUILD_INTERFACE:Backward::Interface>)
if(uOS_ENABLE_TUI)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${FINALCUT_LINK} gpm ncurses rt)
endif()

# Optional CLI / JSON exposure for users
if(uOS_ENABLE_CLI)
    add_library(uOS_cli INTERFACE)
    target_include_directories(uOS_cli INTERFACE ${CLI_INCLUDE_DIR})

    add_library(uOS_json INTERFACE)
    target_include_directories(uOS_json INTERFACE ${JSON_INCLUDE_DIR})
endif()

# INSTALL #
# Omit feature headers that pull optional deps when those features are off
set(UOS_HEADER_EXCLUDES)
if(NOT uOS_ENABLE_TUI)
    list(APPEND UOS_HEADER_EXCLUDES PATTERN "detail/flogview.h" EXCLUDE)
endif()
if(NOT uOS_ENABLE_CLI)
    list(APPEND UOS_HEADER_EXCLUDES PATTERN "detail/json_fwd.h" EXCLUDE)
endif()
install(DIRECTORY ${INCLUDE_DIR}/
        DESTINATION include/uOS
        FILES_MATCHING PATTERN "*.h"
        ${UOS_HEADER_EXCLUDES})
install(DIRECTORY ${quill_SOURCE_DIR}/include/quill
        DESTINATION include/uOS)

# Install the library and export targets
set(UOS_INSTALL_TARGETS ${PROJECT_NAME})
if(uOS_ENABLE_CLI)
    list(APPEND UOS_INSTALL_TARGETS uOS_cli uOS_json)
endif()
install(TARGETS ${UOS_INSTALL_TARGETS}
        EXPORT uOSTargets
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION bin)

install(EXPORT uOSTargets
        FILE uOSTargets.cmake
        NAMESPACE uOS::
        DESTINATION lib/cmake/uOS)

# Generate and install cmake package config
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/uOSConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        "${PROJECT_BINARY_DIR}/uOSConfigVersion.cmake"
        DESTINATION lib/cmake/uOS)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/uOSConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/uOSConfig.cmake"
    INSTALL_DESTINATION lib/cmake/uOS)
    
install(FILES
        "${PROJECT_BINARY_DIR}/uOSConfig.cmake"
        DESTINATION lib/cmake/uOS)

# Generate and install pkg-config file
set(_uos_pc_requires)
set(_uos_pc_libs)
if(uOS_ENABLE_TUI)
    list(APPEND _uos_pc_requires finalcut ncurses)
    list(APPEND _uos_pc_libs -lrt -lgpm)
endif()
if(_uos_bw_backend STREQUAL "dw")
    list(APPEND _uos_pc_requires libdw)
elseif(_uos_bw_backend STREQUAL "bfd")
    list(APPEND _uos_pc_libs -lbfd -ldl)
elseif(_uos_bw_backend STREQUAL "dwarf")
    list(APPEND _uos_pc_libs -ldwarf -lelf -ldl)
endif()
string(REPLACE ";" " " UOS_PC_REQUIRES_PRIVATE "${_uos_pc_requires}")
string(REPLACE ";" " " UOS_PC_LIBS_PRIVATE "${_uos_pc_libs}")
set(UOS_PC_EXTRA_CFLAGS "-DQUILL_DISABLE_NON_PREFIXED_MACROS ")
if(uOS_ENABLE_TUI)
    string(APPEND UOS_PC_EXTRA_CFLAGS "-DuOS_ENABLE_TUI ")
endif()
if(uOS_ENABLE_CLI)
    string(APPEND UOS_PC_EXTRA_CFLAGS "-DuOS_ENABLE_CLI")
endif()
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/pkgconfig/uOS.pc.in"
    "${PROJECT_BINARY_DIR}/uOS.pc"
    @ONLY)

install(FILES "${PROJECT_BINARY_DIR}/uOS.pc"
        DESTINATION lib/pkgconfig)

if(uOS_BUILD_TESTS)
    add_executable(log_stream_test tests/log_stream_test.cpp)
    target_link_libraries(log_stream_test PRIVATE ${PROJECT_NAME})
    add_executable(create_child_test tests/create_child_test.cpp)
    target_link_libraries(create_child_test PRIVATE ${PROJECT_NAME})
endif()



# Task -> Actor
# evaluate timer functionality with grok
# task queue icin moodycamel::ConcurrentQueue dene
# on deleteTask -> task queue'de bulunan eventleri bitirsin mi?
# CLI ve _getStateNameMap tekrar değerlendirilecek. _getStateNameMap kullanılmıyorsa kaldırılacak
# AI uygulama usr/bin klasörüne yüklendiğinde orada LOG klasörü yaratamayacağı için XDG_STATE_HOME diye bir yere koydu logları. Burayı düzelt
# reduce standard requirement to c++17. düşürmek mantıklı değil, zaten yeni projeleri hedefliyoruz. c++20 için genel olarak review yapılmalı

# diğer işlerden önce fark edilmek için example ekle
# sistemdeki eventleri izlemek için bir tool yazılımı yapılacak (AI ile yap)
# --no-signal-handle ve --no-terminal-log ekle
# add fetch and install for asio too
# auto install for dependencies is not recommended. so provide an option to install all or just uOS and make just uOS as rcommended option
# add option for building shared library
# add uninstall
# option(uOS_MODULE "Build a module instead of a traditional library." OFF) --> bunu ekle ve node.js tarzı module yap (örneği FMT'den çaldım)

# add tests
# add examples
# log view finalize
# cmake with examples (add final, fmt, etc. as modules)
