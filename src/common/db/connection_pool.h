#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>

#include <pqxx/pqxx>

namespace jq::db {

// Thread-safe pool of libpqxx connections.
// Connections are acquired via Borrow() and released automatically when the
// returned BorrowedConnection goes out of scope.
class ConnectionPool {
public:
    // connstr: a libpq connection string, e.g.
    //   "host=localhost port=5432 dbname=jobqueue user=jq password=secret"
    explicit ConnectionPool(std::string connstr, std::size_t pool_size = 10);

    // Non-copyable, non-movable.
    ConnectionPool(const ConnectionPool&) = delete;
    ConnectionPool& operator=(const ConnectionPool&) = delete;

    // RAII handle: returns the connection to the pool on destruction.
    class BorrowedConnection {
    public:
        BorrowedConnection(ConnectionPool* pool,
                           std::unique_ptr<pqxx::connection> conn);
        ~BorrowedConnection();

        BorrowedConnection(BorrowedConnection&&) noexcept;
        BorrowedConnection& operator=(BorrowedConnection&&) noexcept;
        BorrowedConnection(const BorrowedConnection&) = delete;
        BorrowedConnection& operator=(const BorrowedConnection&) = delete;

        pqxx::connection& get() { return *conn_; }
        pqxx::connection* operator->() { return conn_.get(); }

    private:
        ConnectionPool* pool_;
        std::unique_ptr<pqxx::connection> conn_;
    };

    // Block until a connection is available, then return it.
    BorrowedConnection Borrow();

    std::size_t pool_size() const { return pool_size_; }

private:
    void Return(std::unique_ptr<pqxx::connection> conn);

    std::string connstr_;
    std::size_t pool_size_;
    std::mutex  mutex_;
    std::condition_variable cv_;
    std::queue<std::unique_ptr<pqxx::connection>> idle_;
};

}  // namespace jq::db
