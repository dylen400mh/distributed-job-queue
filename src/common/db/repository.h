#pragma once

#include "common/db/connection_pool.h"

namespace jq::db {

// Base class for all repository types.
// Subclasses borrow a connection from the pool to execute queries.
// No raw SQL should appear outside of repository subclasses.
class Repository {
public:
    explicit Repository(ConnectionPool& pool) : pool_(pool) {}
    virtual ~Repository() = default;

    // Non-copyable; repositories are lightweight view objects.
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

protected:
    // Borrow a connection for the duration of a single operation.
    ConnectionPool::BorrowedConnection Conn() { return pool_.Borrow(); }

private:
    ConnectionPool& pool_;
};

}  // namespace jq::db
