#pragma once

#include <filesystem>
#include <string>

#include "common/db/connection_pool.h"

namespace jq::db {

// Apply all pending Flyway-named migration files (V{n}__{desc}.sql) from
// migrations_dir to the database reachable via pool.
//
// Tracks applied migrations in a schema_migrations table (created if absent).
// Migrations are applied in ascending version order inside individual
// transactions.  Throws std::runtime_error on any failure.
void RunMigrations(ConnectionPool& pool,
                   const std::filesystem::path& migrations_dir);

}  // namespace jq::db
