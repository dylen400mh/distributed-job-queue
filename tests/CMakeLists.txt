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
