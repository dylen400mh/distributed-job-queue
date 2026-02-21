cmake_minimum_required(VERSION 3.20)
project(distributed-job-queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE tooling
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Homebrew prefix (macOS): ensure find_package searches Homebrew paths
# ---------------------------------------------------------------------------
if(APPLE)
    list(PREPEND CMAKE_PREFIX_PATH /opt/homebrew)
    # pkg-config — include opt/* paths for keg-only Homebrew formulas
    set(ENV{PKG_CONFIG_PATH}
        "/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/libpq/lib/pkgconfig:/opt/homebrew/opt/libpqxx/lib/pkgconfig:/opt/homebrew/opt/hiredis/lib/pkgconfig:/opt/homebrew/opt/librdkafka/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}"
    )
endif()

# ---------------------------------------------------------------------------
# Dependencies — packages with CMake config files
# ---------------------------------------------------------------------------
find_package(gRPC CONFIG REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# Boost.System is header-only in Boost >= 1.69; only request filesystem.
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(GTest CONFIG REQUIRED)
find_package(prometheus-cpp CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Dependencies — via PkgConfig (no CMake config provided by Homebrew)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBPQXX REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
pkg_check_modules(RDKAFKA REQUIRED IMPORTED_TARGET rdkafka++)

# ---------------------------------------------------------------------------
# Common dependency set (linked by all three binaries)
# ---------------------------------------------------------------------------
set(COMMON_LIBS
    gRPC::grpc++
    gRPC::grpc
    protobuf::libprotobuf
    absl::base
    absl::strings
    absl::synchronization
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    Boost::filesystem
    Boost::headers
    prometheus-cpp::pull
    prometheus-cpp::core
    PkgConfig::LIBPQXX
    PkgConfig::HIREDIS
    PkgConfig::RDKAFKA
)

# ---------------------------------------------------------------------------
# jq-server
# ---------------------------------------------------------------------------
add_executable(jq-server src/server/main.cc)
target_include_directories(jq-server PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq-server PRIVATE ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# jq-worker
# ---------------------------------------------------------------------------
add_executable(jq-worker src/worker/main.cc)
target_include_directories(jq-worker PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq-worker PRIVATE ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# jq-ctl
# ---------------------------------------------------------------------------
add_executable(jq-ctl src/ctl/main.cc)
target_include_directories(jq-ctl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq-ctl PRIVATE ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
