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
# Protobuf + gRPC code generation
# ---------------------------------------------------------------------------
find_program(PROTOC_EXE protoc HINTS /opt/homebrew/bin REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin HINTS /opt/homebrew/bin REQUIRED)

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

    add_custom_command(
        OUTPUT  ${PB_SRC} ${PB_HDR} ${GRPC_SRC} ${GRPC_HDR}
        COMMAND ${PROTOC_EXE}
                --proto_path=${PROTO_DIR}
                --proto_path=/opt/homebrew/include
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
target_link_libraries(proto_gen PUBLIC gRPC::grpc++ protobuf::libprotobuf)

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
    jq_db
    proto_gen
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
