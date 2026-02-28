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
else()
    # Debian/Ubuntu: cmake config files (gRPC, protobuf, abseil, Boost, …) are
    # installed under the architecture-specific lib directory, which CMake does
    # not search automatically.
    list(PREPEND CMAKE_PREFIX_PATH /usr/lib/x86_64-linux-gnu)
endif()

# ---------------------------------------------------------------------------
# Dependencies — packages with CMake config files
# ---------------------------------------------------------------------------
find_package(absl CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
# Boost.System is header-only in Boost >= 1.69; only request filesystem.
find_package(Boost CONFIG REQUIRED COMPONENTS filesystem)
find_package(prometheus-cpp CONFIG REQUIRED)

option(BUILD_TESTS "Build unit and integration tests" ON)
if(BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
endif()

# ---------------------------------------------------------------------------
# Dependencies — via PkgConfig
# gRPC and Protobuf use pkg-config on both macOS (Homebrew) and Linux
# (Ubuntu apt). Neither ships cmake config files in standard apt packages.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(GRPC_PP  REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(LIBPQXX  REQUIRED IMPORTED_TARGET libpqxx)
pkg_check_modules(HIREDIS  REQUIRED IMPORTED_TARGET hiredis)
pkg_check_modules(RDKAFKA  REQUIRED IMPORTED_TARGET rdkafka++)
pkg_check_modules(YAML_CPP REQUIRED IMPORTED_TARGET yaml-cpp)

# ---------------------------------------------------------------------------
# Protobuf + gRPC code generation
# ---------------------------------------------------------------------------
find_program(PROTOC_EXE protoc HINTS /opt/homebrew/bin /usr/bin REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin HINTS /opt/homebrew/bin /usr/bin REQUIRED)

set(PROTO_DIR     ${CMAKE_SOURCE_DIR}/proto)
set(PROTO_OUT_DIR ${CMAKE_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${PROTO_OUT_DIR})

set(PROTO_FILES
    common.proto
    job_service.proto
    worker_service.proto
    admin_service.proto
)

set(PROTO_GEN_SRCS "")

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    set(PB_SRC   ${PROTO_OUT_DIR}/${PROTO_NAME}.pb.cc)
    set(PB_HDR   ${PROTO_OUT_DIR}/${PROTO_NAME}.pb.h)
    set(GRPC_SRC ${PROTO_OUT_DIR}/${PROTO_NAME}.grpc.pb.cc)
    set(GRPC_HDR ${PROTO_OUT_DIR}/${PROTO_NAME}.grpc.pb.h)

    # On macOS the well-known protobuf types live under the Homebrew prefix.
    # On Linux they are in /usr/include, which protoc searches by default.
    if(APPLE)
        set(PROTOC_EXTRA_PATH --proto_path=/opt/homebrew/include)
    else()
        set(PROTOC_EXTRA_PATH "")
    endif()

    add_custom_command(
        OUTPUT  ${PB_SRC} ${PB_HDR} ${GRPC_SRC} ${GRPC_HDR}
        COMMAND ${PROTOC_EXE}
                --proto_path=${PROTO_DIR}
                ${PROTOC_EXTRA_PATH}
                --cpp_out=${PROTO_OUT_DIR}
                --grpc_out=${PROTO_OUT_DIR}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
                ${PROTO_DIR}/${PROTO_FILE}
        DEPENDS ${PROTO_DIR}/${PROTO_FILE}
        COMMENT "Generating protobuf/gRPC C++ for ${PROTO_FILE}"
    )

    list(APPEND PROTO_GEN_SRCS ${PB_SRC} ${GRPC_SRC})
endforeach()

# Static library of all generated proto/gRPC sources.
add_library(proto_gen STATIC ${PROTO_GEN_SRCS})
target_include_directories(proto_gen PUBLIC ${PROTO_OUT_DIR})
target_link_libraries(proto_gen PUBLIC PkgConfig::GRPC_PP PkgConfig::PROTOBUF)

# ---------------------------------------------------------------------------
# jq_config — YAML config loader + validator
# ---------------------------------------------------------------------------
add_library(jq_config STATIC
    src/common/config/config.cc
)
target_include_directories(jq_config PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_config PUBLIC PkgConfig::YAML_CPP)

# ---------------------------------------------------------------------------
# jq_log — structured JSON logger
# ---------------------------------------------------------------------------
add_library(jq_log STATIC
    src/common/logging/logger.cc
)
target_include_directories(jq_log PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_log PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# jq_metrics — prometheus-cpp metric definitions (FR-040)
# ---------------------------------------------------------------------------
add_library(jq_metrics STATIC
    src/common/metrics/metrics.cc
)
target_include_directories(jq_metrics PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_metrics PUBLIC
    prometheus-cpp::pull
    prometheus-cpp::core
)

# ---------------------------------------------------------------------------
# jq_redis — hiredis-backed RedisClient + DistributedLock
# ---------------------------------------------------------------------------
add_library(jq_redis STATIC
    src/common/redis/redis_client.cc
)
target_include_directories(jq_redis PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_redis PUBLIC
    jq_metrics
    jq_log
    jq_config
    PkgConfig::HIREDIS
)

# ---------------------------------------------------------------------------
# jq_kafka — librdkafka producer + consumer wrappers
# ---------------------------------------------------------------------------
add_library(jq_kafka STATIC
    src/common/kafka/kafka_producer.cc
    src/common/kafka/kafka_consumer.cc
)
target_include_directories(jq_kafka PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_kafka PUBLIC
    jq_metrics
    jq_log
    jq_config
    PkgConfig::RDKAFKA
)

# ---------------------------------------------------------------------------
# jq_db — shared database layer (connection pool, repository base, migrations)
# ---------------------------------------------------------------------------
add_library(jq_db STATIC
    src/common/db/connection_pool.cc
    src/common/db/repository.cc
    src/common/db/migrations.cc
)
target_include_directories(jq_db PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_db PUBLIC PkgConfig::LIBPQXX)

# ---------------------------------------------------------------------------
# Common dependency set (linked by all three binaries)
# ---------------------------------------------------------------------------
set(COMMON_LIBS
    jq_config
    jq_log
    jq_db
    jq_metrics
    jq_redis
    jq_kafka
    proto_gen
    PkgConfig::GRPC_PP
    PkgConfig::PROTOBUF
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
# jq_server_db — server-side job + worker + queue repositories
# ---------------------------------------------------------------------------
add_library(jq_server_db STATIC
    src/server/db/job_repository.cc
    src/server/db/worker_repository.cc
    src/server/db/queue_repository.cc
)
target_include_directories(jq_server_db PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_server_db PUBLIC jq_db jq_log PkgConfig::LIBPQXX)

# ---------------------------------------------------------------------------
# jq_server_scheduler — WorkerRegistry + Scheduler loop
# ---------------------------------------------------------------------------
add_library(jq_server_scheduler STATIC
    src/server/scheduler/worker_registry.cc
    src/server/scheduler/scheduler.cc
)
target_include_directories(jq_server_scheduler PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_OUT_DIR}
)
target_link_libraries(jq_server_scheduler PUBLIC
    jq_server_db
    jq_redis
    jq_kafka
    jq_metrics
    jq_log
    proto_gen
    PkgConfig::GRPC_PP
    PkgConfig::PROTOBUF
)

# ---------------------------------------------------------------------------
# jq_server_grpc — JobService, WorkerService, AdminService + GrpcServer
# ---------------------------------------------------------------------------
add_library(jq_server_grpc STATIC
    src/server/grpc/job_service_impl.cc
    src/server/grpc/worker_service_impl.cc
    src/server/grpc/admin_service_impl.cc
    src/server/grpc/server.cc
)
target_include_directories(jq_server_grpc PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_OUT_DIR}
)
target_link_libraries(jq_server_grpc PUBLIC
    jq_server_db
    jq_server_scheduler
    jq_kafka
    jq_log
    proto_gen
    PkgConfig::GRPC_PP
    PkgConfig::PROTOBUF
)

# ---------------------------------------------------------------------------
# jq_server_health — HTTP /healthz + /readyz server
# ---------------------------------------------------------------------------
add_library(jq_server_health STATIC
    src/server/health/health_server.cc
)
target_include_directories(jq_server_health PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq_server_health PUBLIC jq_db jq_redis jq_kafka jq_log PkgConfig::LIBPQXX)

# ---------------------------------------------------------------------------
# jq-server
# ---------------------------------------------------------------------------
add_executable(jq-server src/server/main.cc)
target_include_directories(jq-server PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq-server PRIVATE
    jq_server_grpc
    jq_server_health
    ${COMMON_LIBS}
)

# ---------------------------------------------------------------------------
# jq_worker_lib — Worker + JobExecutor (separate lib for testability)
# ---------------------------------------------------------------------------
add_library(jq_worker_lib STATIC
    src/worker/worker.cc
    src/worker/job_executor.cc
)
target_include_directories(jq_worker_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_OUT_DIR}
)
target_link_libraries(jq_worker_lib PUBLIC
    ${COMMON_LIBS}
)

# ---------------------------------------------------------------------------
# jq-worker
# ---------------------------------------------------------------------------
add_executable(jq-worker src/worker/main.cc)
target_include_directories(jq-worker PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq-worker PRIVATE jq_worker_lib ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# jq_ctl_lib — GrpcClient + Formatter + all command handlers
# ---------------------------------------------------------------------------
add_library(jq_ctl_lib STATIC
    src/ctl/client.cc
    src/ctl/output/formatter.cc
    src/ctl/commands/jobs.cc
    src/ctl/commands/queues.cc
    src/ctl/commands/workers.cc
    src/ctl/commands/system.cc
)
target_include_directories(jq_ctl_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PROTO_OUT_DIR}
)
target_link_libraries(jq_ctl_lib PUBLIC
    proto_gen
    PkgConfig::GRPC_PP
    PkgConfig::PROTOBUF
    nlohmann_json::nlohmann_json
    jq_config
)

# ---------------------------------------------------------------------------
# jq-ctl
# ---------------------------------------------------------------------------
add_executable(jq-ctl src/ctl/main.cc)
target_include_directories(jq-ctl PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(jq-ctl PRIVATE jq_ctl_lib ${COMMON_LIBS})

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
