#include <cstdlib>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>
#include <pqxx/pqxx>

#include "common/db/connection_pool.h"
#include "common/db/migrations.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string TestConnStr() {
    // Allow override via environment variable for CI.
    const char* env = std::getenv("JQ_TEST_DB");
    if (env && *env) return std::string(env);
    return "host=localhost port=5432 dbname=jq_test user=jq_test password=jq_test";
}

static std::filesystem::path MigrationsDir() {
    // Walk up from the binary location until we find db/migrations/.
    // In practice, tests are run from the build directory.
    std::filesystem::path p = std::filesystem::current_path();
    for (int i = 0; i < 5; ++i) {
        auto candidate = p / "db" / "migrations";
        if (std::filesystem::is_directory(candidate)) return candidate;
        p = p.parent_path();
    }
    return "";
}

// ---------------------------------------------------------------------------
// Fixture: connects to the test DB; skips if unavailable.
// ---------------------------------------------------------------------------

class MigrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        connstr_ = TestConnStr();
        try {
            pqxx::connection probe(connstr_);
            (void)probe;
        } catch (const std::exception& e) {
            GTEST_SKIP() << "Test database unavailable (" << e.what()
                         << "). Set JQ_TEST_DB to enable.";
        }

        migrations_dir_ = MigrationsDir();
        ASSERT_FALSE(migrations_dir_.empty())
            << "Could not locate db/migrations/ directory";

        // Drop and recreate the test schema for a clean slate.
        pqxx::connection conn(connstr_);
        pqxx::work txn(conn);
        txn.exec("DROP SCHEMA public CASCADE");
        txn.exec("CREATE SCHEMA public");
        txn.commit();
    }

    std::string connstr_;
    std::filesystem::path migrations_dir_;
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(MigrationTest, RunMigrationsAppliesAllFiveMigrations) {
    jq::db::ConnectionPool pool(connstr_, 2);
    ASSERT_NO_THROW(jq::db::RunMigrations(pool, migrations_dir_));

    // Verify all expected tables exist.
    auto borrowed = pool.Borrow();
    pqxx::work txn(borrowed.get());

    auto table_exists = [&](const std::string& name) {
        auto row = txn.exec(
            "SELECT COUNT(*) FROM information_schema.tables "
            "WHERE table_schema = 'public' AND table_name = " +
            txn.quote(name)).one_row();
        return row[0].as<int>() == 1;
    };

    EXPECT_TRUE(table_exists("queues"));
    EXPECT_TRUE(table_exists("workers"));
    EXPECT_TRUE(table_exists("jobs"));
    EXPECT_TRUE(table_exists("job_events"));
    EXPECT_TRUE(table_exists("schema_migrations"));
    txn.commit();
}

TEST_F(MigrationTest, DefaultQueueSeeded) {
    jq::db::ConnectionPool pool(connstr_, 2);
    jq::db::RunMigrations(pool, migrations_dir_);

    auto borrowed = pool.Borrow();
    pqxx::work txn(borrowed.get());
    auto row = txn.exec("SELECT name FROM queues WHERE name = 'default'").one_row();
    EXPECT_EQ(row[0].as<std::string>(), "default");
    txn.commit();
}

TEST_F(MigrationTest, RunMigrationsIsIdempotent) {
    jq::db::ConnectionPool pool(connstr_, 2);
    // Running twice must not throw or duplicate rows.
    ASSERT_NO_THROW(jq::db::RunMigrations(pool, migrations_dir_));
    ASSERT_NO_THROW(jq::db::RunMigrations(pool, migrations_dir_));

    auto borrowed = pool.Borrow();
    pqxx::work txn(borrowed.get());
    auto row = txn.exec("SELECT COUNT(*) FROM schema_migrations").one_row();
    EXPECT_EQ(row[0].as<int>(), 5);
    txn.commit();
}
