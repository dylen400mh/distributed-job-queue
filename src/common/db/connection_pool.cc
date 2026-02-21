#include "common/db/connection_pool.h"

#include <stdexcept>

namespace jq::db {

ConnectionPool::ConnectionPool(std::string connstr, std::size_t pool_size)
    : connstr_(std::move(connstr)), pool_size_(pool_size) {
    for (std::size_t i = 0; i < pool_size_; ++i) {
        idle_.push(std::make_unique<pqxx::connection>(connstr_));
    }
}

ConnectionPool::BorrowedConnection ConnectionPool::Borrow() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !idle_.empty(); });
    auto conn = std::move(idle_.front());
    idle_.pop();
    return BorrowedConnection(this, std::move(conn));
}

void ConnectionPool::Return(std::unique_ptr<pqxx::connection> conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Attempt reconnect if connection dropped.
        if (!conn->is_open()) {
            conn = std::make_unique<pqxx::connection>(connstr_);
        }
        idle_.push(std::move(conn));
    }
    cv_.notify_one();
}

// ---------------------------------------------------------------------------
// BorrowedConnection
// ---------------------------------------------------------------------------

ConnectionPool::BorrowedConnection::BorrowedConnection(
    ConnectionPool* pool, std::unique_ptr<pqxx::connection> conn)
    : pool_(pool), conn_(std::move(conn)) {}

ConnectionPool::BorrowedConnection::~BorrowedConnection() {
    if (pool_ && conn_) {
        pool_->Return(std::move(conn_));
    }
}

ConnectionPool::BorrowedConnection::BorrowedConnection(
    BorrowedConnection&& other) noexcept
    : pool_(other.pool_), conn_(std::move(other.conn_)) {
    other.pool_ = nullptr;
}

ConnectionPool::BorrowedConnection&
ConnectionPool::BorrowedConnection::operator=(
    BorrowedConnection&& other) noexcept {
    if (this != &other) {
        if (pool_ && conn_) pool_->Return(std::move(conn_));
        pool_      = other.pool_;
        conn_      = std::move(other.conn_);
        other.pool_ = nullptr;
    }
    return *this;
}

}  // namespace jq::db
