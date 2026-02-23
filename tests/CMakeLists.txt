# tests/CMakeLists.txt

# ---------------------------------------------------------------------------
# DB unit tests (require a running PostgreSQL instance; skipped automatically
# if the test database is unreachable).
# Set JQ_TEST_DB env var to override the connection string.
# ---------------------------------------------------------------------------
add_executable(db_unit_tests
    unit/db/migration_test.cc
)
target_include_directories(db_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(db_unit_tests PRIVATE
    jq_db
    GTest::gtest_main
)
add_test(NAME db_unit_tests COMMAND db_unit_tests)

# ---------------------------------------------------------------------------
# Config unit tests (no external services required; runs anywhere)
# ---------------------------------------------------------------------------
add_executable(config_unit_tests
    unit/config/config_test.cc
)
target_include_directories(config_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(config_unit_tests PRIVATE
    jq_config
    GTest::gtest_main
)
add_test(NAME config_unit_tests COMMAND config_unit_tests)

# ---------------------------------------------------------------------------
# Redis unit tests — DistributedLock RAII behaviour (no Redis required)
# ---------------------------------------------------------------------------
add_executable(redis_unit_tests
    unit/redis/distributed_lock_test.cc
)
target_include_directories(redis_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(redis_unit_tests PRIVATE
    jq_redis
    GTest::gtest_main
    GTest::gmock
)
add_test(NAME redis_unit_tests COMMAND redis_unit_tests)

# ---------------------------------------------------------------------------
# Kafka unit tests — delivery report error path (no Kafka required)
# ---------------------------------------------------------------------------
add_executable(kafka_unit_tests
    unit/kafka/kafka_delivery_test.cc
)
target_include_directories(kafka_unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(kafka_unit_tests PRIVATE
    jq_metrics
    GTest::gtest_main
)
add_test(NAME kafka_unit_tests COMMAND kafka_unit_tests)

# ---------------------------------------------------------------------------
# Server unit tests — JobService RPC logic (no DB or Kafka required)
# ---------------------------------------------------------------------------
add_executable(server_unit_tests
    unit/server/job_service_test.cc
)
target_include_directories(server_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/proto_gen
)
target_link_libraries(server_unit_tests PRIVATE
    jq_server_grpc
    jq_server_db
    jq_kafka
    jq_metrics
    jq_log
    proto_gen
    gRPC::grpc++
    protobuf::libprotobuf
    GTest::gtest_main
    GTest::gmock
)
add_test(NAME server_unit_tests COMMAND server_unit_tests)

# ---------------------------------------------------------------------------
# Scheduler unit tests — backoff formula, WorkerRegistry, heartbeat logic
# (no external services required)
# ---------------------------------------------------------------------------
add_executable(scheduler_unit_tests
    unit/server/scheduler_test.cc
)
target_include_directories(scheduler_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/proto_gen
)
target_link_libraries(scheduler_unit_tests PRIVATE
    jq_server_scheduler
    jq_server_db
    jq_kafka
    jq_metrics
    jq_log
    proto_gen
    gRPC::grpc++
    protobuf::libprotobuf
    absl::base
    absl::strings
    absl::synchronization
    GTest::gtest_main
    GTest::gmock
)
add_test(NAME scheduler_unit_tests COMMAND scheduler_unit_tests)
