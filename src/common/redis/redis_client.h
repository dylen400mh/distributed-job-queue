#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Forward-declare hiredis context to avoid pulling the header into every TU.
struct redisContext;

namespace jq {

struct RedisConfig;  // from common/config/config.h

// ---------------------------------------------------------------------------
// IRedisLockable — minimal interface used by DistributedLock.
// Extracted so tests can inject a mock without a real Redis server.
// ---------------------------------------------------------------------------
class IRedisLockable {
public:
    virtual ~IRedisLockable() = default;
    virtual bool SetNxPx(const std::string& key,
                         const std::string& value,
                         int64_t            ttl_ms) = 0;
    virtual void Del(const std::string& key) = 0;
};

// ---------------------------------------------------------------------------
// RedisClient — hiredis-backed thin wrapper.
//
// Thread safety: NOT thread-safe. Each thread should own its own instance.
// All methods catch hiredis errors/timeouts, log a warning, and return
// sensible defaults so Redis unavailability never crashes the process.
// ---------------------------------------------------------------------------
class RedisClient : public IRedisLockable {
public:
    explicit RedisClient(const RedisConfig& cfg);
    ~RedisClient();

    // Non-copyable; movable.
    RedisClient(const RedisClient&)            = delete;
    RedisClient& operator=(const RedisClient&) = delete;
    RedisClient(RedisClient&&)                 = default;
    RedisClient& operator=(RedisClient&&)      = default;

    // SET key value PX ttl_ms NX — returns true if the key was set (lock acquired).
    bool SetNxPx(const std::string& key,
                 const std::string& value,
                 int64_t            ttl_ms) override;

    // DEL key
    void Del(const std::string& key) override;

    // GET key — returns nullopt if key missing or Redis unavailable.
    std::optional<std::string> Get(const std::string& key);

    // SET key value PX ttl_ms
    void Set(const std::string& key,
             const std::string& value,
             int64_t            ttl_ms);

    // INCR key — returns 0 on error.
    int64_t Incr(const std::string& key);

    // PEXPIRE key ttl_ms
    void Expire(const std::string& key, int64_t ttl_ms);

    // Returns true if the underlying connection is alive.
    bool IsConnected() const;

private:
    // Attempt to (re)connect.  Returns false on failure.
    bool Connect();

    // Execute a hiredis command and return the raw reply, or nullptr on error.
    // Handles disconnection and logs errors.
    void* ExecCommand(const char* fmt, ...);

    redisContext* ctx_   = nullptr;
    std::string   host_;
    int           port_  = 6379;
    std::string   password_;
    int           db_    = 0;
    int           connect_timeout_ms_ = 1000;
};

// ---------------------------------------------------------------------------
// DistributedLock — RAII wrapper around RedisClient::SetNxPx / Del.
//
// Acquires lock:job:<key> on construction; throws std::runtime_error if the
// lock cannot be acquired (another replica holds it).
// Releases the lock automatically on destruction.
// ---------------------------------------------------------------------------
class DistributedLock {
public:
    // Acquire the lock.  Throws std::runtime_error if unavailable.
    DistributedLock(IRedisLockable&    client,
                    const std::string& key,
                    const std::string& owner_id,
                    int64_t            ttl_ms = 60'000);

    // Release the lock.
    ~DistributedLock();

    // Non-copyable, non-movable (holds reference to client).
    DistributedLock(const DistributedLock&)            = delete;
    DistributedLock& operator=(const DistributedLock&) = delete;
    DistributedLock(DistributedLock&&)                 = delete;
    DistributedLock& operator=(DistributedLock&&)      = delete;

    const std::string& LockKey() const { return lock_key_; }

private:
    IRedisLockable& client_;
    std::string  lock_key_;
    bool         acquired_ = false;
};

}  // namespace jq
