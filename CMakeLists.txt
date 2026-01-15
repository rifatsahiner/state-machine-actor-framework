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
    ${SRC_DIR}/logger.cpp
    ${SRC_DIR}/flogview.cpp)

# Create library
add_library(${PROJECT_NAME} STATIC ${SOURCES})

# Include the FetchContent module
include(FetchContent)

############ ASIO ############
# Check for ASIO header
find_path(ASIO_INCLUDE_DIR NAMES asio.hpp
          PATH_SUFFIXES asio
          PATHS
            /usr/include
            /usr/local/include
            ${CMAKE_INCLUDE_PATH})

# Check if ASIO was found
if(ASIO_INCLUDE_DIR)
    message(STATUS "ASIO found: ${ASIO_INCLUDE_DIR}")
else()
    message(FATAL_ERROR "ASIO not found. Please install ASIO or specify its location.")
endif()

############ CLI ############
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

############ JSON ############
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

############ FMT ############ update!!
find_package(fmt 11.1 QUIET)
if(fmt_FOUND)
    message(STATUS "fmt found via find_package")
else()
    message(STATUS "fmt not found. Fetching from source.")
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt
        GIT_TAG 11.2.0
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(FMT_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(fmt)
endif()

############ FINALCUT ############
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

############ BACKWARD ############
# check for libdw
find_library(DW_LIB dw)
if(NOT DW_LIB)
    message(FATAL_ERROR "libdw not found. Please install libdw (e.g., elfutils).")
endif()

find_path(BW_INCLUDE_DIR
    NAMES backward.hpp
    PATHS /usr/include /usr/local/include ${CMAKE_INCLUDE_PATH}
)
if(BW_INCLUDE_DIR)
    message(STATUS "Backward found at ${BW_INCLUDE_DIR}")
else()
    # fetch backward-cpp
    FetchContent_Declare(backward
        GIT_REPOSITORY  https://github.com/bombela/backward-cpp
        GIT_TAG         master)

    FetchContent_Populate(backward)  # Only populate; do not make available
    set(BW_INCLUDE_DIR ${backward_SOURCE_DIR})
endif()

# Include directories 
target_include_directories(${PROJECT_NAME} PRIVATE ${FINALCUT_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${BW_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${ASIO_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${CLI_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${JSON_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PUBLIC
    $<BUILD_INTERFACE:${INCLUDE_DIR}>
    $<INSTALL_INTERFACE:include/uOS>)

# link libraries
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME} PUBLIC Threads::Threads)
target_link_libraries(${PROJECT_NAME} PUBLIC fmt::fmt)
target_link_libraries(${PROJECT_NAME} PRIVATE ${DW_LIB})
target_link_libraries(${PROJECT_NAME} PRIVATE ${FINALCUT_LINK} gpm ncurses rt)

# Optional CLI exposure for users
add_library(uOS_cli INTERFACE)
target_include_directories(uOS_cli INTERFACE ${CLI_INCLUDE_DIR})

# Optional JSON exposure for users
add_library(uOS_json INTERFACE)
target_include_directories(uOS_json INTERFACE ${JSON_INCLUDE_DIR})

# INSTALL #
# Install headers
install(DIRECTORY ${INCLUDE_DIR}/
        DESTINATION include/uOS
        FILES_MATCHING PATTERN "*.h")

# Install the library and export targets
install(TARGETS ${PROJECT_NAME} uOS_cli uOS_json
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
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/pkgconfig/uOS.pc.in"
    "${PROJECT_BINARY_DIR}/uOS.pc"
    @ONLY)

install(FILES "${PROJECT_BINARY_DIR}/uOS.pc"
        DESTINATION lib/pkgconfig)



# tum loglama isini spdlog'a cevir
# reset durumunda (segfault, sigabort vs.) stdout ve stderr flush yapacak kodu ekle -> spdlog'a yaptir
# set FW and console debug level from CLI -> suanda trace loglar dosyaya ve console'a yazilmiyor. bu seviye global olarak belirlenecek
    # tui log seviyesi ayrica belirlenecek. basilacak bir log iki seviyeden de kucukse log kodlarına hic girilmeden return edilecek
    # bunlarin default degerleri info olacak, baslangicta --log-level ile set edilebilecek
# bir klasorde 10k yada 20k dan fazla log dosyası olmasını engelle, 33k dosya varken bu sekilde bir hata aldim
# task queue icin moodycamel::ConcurrentQueue dene
# add fetch and install for asio too
# make compile options for tui and cli -> "If you split into components (e.g., core, tui), use COMPONENTS in find_package and separate exports"
# auto install for dependencies is not recommended. so provide an option to install all or just uOS and make just uOS as rcommended option
# add option for building shared library
# add uninstall
# option(uOS_MODULE "Build a module instead of a traditional library." OFF) --> bunu ekle ve node.js tarzı module yap (örneği FMT'den çaldım)
# add tests
# add examples
# log view finalize
# cmake with examples (add final, fmt, etc. as modules)
