#include "common/db/migrations.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <pqxx/pqxx>

namespace jq::db {

namespace {

// Parse the version number from a Flyway filename: V{n}__{desc}.sql -> n
// Returns 0 if the filename does not match the pattern.
int ParseVersion(const std::string& filename) {
    static const std::regex kPattern(R"(^V(\d+)__.*\.sql$)");
    std::smatch m;
    if (!std::regex_match(filename, m, kPattern)) return 0;
    return std::stoi(m[1].str());
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("Cannot open migration file: " +
                                 path.string());
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void EnsureMigrationsTable(pqxx::connection& conn) {
    pqxx::work txn(conn);
    txn.exec(R"(
        CREATE TABLE IF NOT EXISTS schema_migrations (
            version     INTEGER     NOT NULL,
            description TEXT        NOT NULL,
            applied_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
            CONSTRAINT schema_migrations_pkey PRIMARY KEY (version)
        )
    )");
    txn.commit();
}

std::set<int> AppliedVersions(pqxx::connection& conn) {
    pqxx::work txn(conn);
    auto rows = txn.exec("SELECT version FROM schema_migrations ORDER BY version");
    txn.commit();
    std::set<int> versions;
    for (auto const& row : rows) {
        versions.insert(row[0].as<int>());
    }
    return versions;
}

}  // namespace

void RunMigrations(ConnectionPool& pool,
                   const std::filesystem::path& migrations_dir) {
    if (!std::filesystem::is_directory(migrations_dir)) {
        throw std::runtime_error("Migrations directory not found: " +
                                 migrations_dir.string());
    }

    // Collect and sort migration files by version number.
    struct Migration {
        int         version;
        std::string description;
        std::filesystem::path path;
    };
    std::vector<Migration> migrations;

    for (auto const& entry :
         std::filesystem::directory_iterator(migrations_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string filename = entry.path().filename().string();
        const int version = ParseVersion(filename);
        if (version == 0) continue;

        // Extract description from filename: V{n}__{desc}.sql -> desc
        const std::string stem = entry.path().stem().string();
        const auto sep = stem.find("__");
        const std::string desc =
            (sep != std::string::npos) ? stem.substr(sep + 2) : stem;

        migrations.push_back({version, desc, entry.path()});
    }

    std::sort(migrations.begin(), migrations.end(),
              [](const Migration& a, const Migration& b) {
                  return a.version < b.version;
              });

    auto borrowed = pool.Borrow();
    auto& conn    = borrowed.get();

    EnsureMigrationsTable(conn);
    const auto applied = AppliedVersions(conn);

    for (const auto& m : migrations) {
        if (applied.count(m.version)) continue;  // already applied

        const std::string sql = ReadFile(m.path);

        // Apply the migration SQL and record it — all in one transaction.
        pqxx::work txn(conn);
        txn.exec(sql);
        txn.exec("INSERT INTO schema_migrations (version, description) VALUES (" +
                 std::to_string(m.version) + ", " + txn.quote(m.description) + ")");
        txn.commit();
    }
}

}  // namespace jq::db
