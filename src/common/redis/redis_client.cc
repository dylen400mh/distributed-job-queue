#include "common/redis/redis_client.h"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <hiredis.h>

#include "common/config/config.h"
#include "common/logging/logger.h"
#include "common/metrics/metrics.h"

namespace jq {

namespace {

// Parse "host:port" from the addr string; returns {"localhost", 6379} on error.
std::pair<std::string, int> ParseAddr(const std::string& addr) {
    auto colon = addr.rfind(':');
    if (colon == std::string::npos) {
        return {addr, 6379};
    }
    try {
        int port = std::stoi(addr.substr(colon + 1));
        return {addr.substr(0, colon), port};
    } catch (...) {
        return {addr, 6379};
    }
}

// RAII wrapper around redisReply* — frees reply on destruction.
struct Reply {
    redisReply* ptr = nullptr;
    explicit Reply(void* p) : ptr(static_cast<redisReply*>(p)) {}
    ~Reply() { if (ptr) freeReplyObject(ptr); }
    Reply(const Reply&)            = delete;
    Reply& operator=(const Reply&) = delete;
    bool ok() const { return ptr != nullptr; }
    bool is_string() const { return ok() && ptr->type == REDIS_REPLY_STRING; }
    bool is_integer() const { return ok() && ptr->type == REDIS_REPLY_INTEGER; }
    bool is_status() const { return ok() && ptr->type == REDIS_REPLY_STATUS; }
    bool is_nil() const { return ok() && ptr->type == REDIS_REPLY_NIL; }
    bool is_error() const { return !ok() || ptr->type == REDIS_REPLY_ERROR; }
};

// Measure and observe a Redis operation duration via the global histogram.
struct OpTimer {
    std::string op;
    std::chrono::steady_clock::time_point start;

    explicit OpTimer(std::string operation)
        : op(std::move(operation))
        , start(std::chrono::steady_clock::now()) {}

    ~OpTimer() {
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();
        metrics::RedisOperationDuration()
            .Add({{"operation", op}},
                 prometheus::Histogram::BucketBoundaries{
                     0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0})
            .Observe(elapsed);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// RedisClient
// ---------------------------------------------------------------------------

RedisClient::RedisClient(const RedisConfig& cfg) {
    auto [h, p] = ParseAddr(cfg.addr);
    host_               = h;
    port_               = p;
    password_           = cfg.password;
    db_                 = cfg.db;
    connect_timeout_ms_ = cfg.connect_timeout_ms;
    Connect();
}

RedisClient::~RedisClient() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }
}

bool RedisClient::Connect() {
    if (ctx_) {
        redisFree(ctx_);
        ctx_ = nullptr;
    }

    timeval tv{};
    tv.tv_sec  = connect_timeout_ms_ / 1000;
    tv.tv_usec = (connect_timeout_ms_ % 1000) * 1000;

    ctx_ = redisConnectWithTimeout(host_.c_str(), port_, tv);
    if (!ctx_ || ctx_->err) {
        std::string err = ctx_ ? ctx_->errstr : "allocation failure";
        LOG_WARN("Redis connection failed", {{"host", host_}, {"port", port_}, {"error", err}});
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        return false;
    }

    // AUTH if password is set
    if (!password_.empty()) {
        Reply r(redisCommand(ctx_, "AUTH %s", password_.c_str()));
        if (r.is_error()) {
            LOG_WARN("Redis AUTH failed",
                     {{"error", r.ptr ? r.ptr->str : "no reply"}});
            redisFree(ctx_);
            ctx_ = nullptr;
            return false;
        }
    }

    // SELECT db
    if (db_ != 0) {
        Reply r(redisCommand(ctx_, "SELECT %d", db_));
        if (r.is_error()) {
            LOG_WARN("Redis SELECT failed",
                     {{"db", db_}, {"error", r.ptr ? r.ptr->str : "no reply"}});
            redisFree(ctx_); ctx_ = nullptr;
            return false;
        }
    }

    return true;
}

bool RedisClient::IsConnected() const {
    return ctx_ != nullptr && ctx_->err == 0;
}

// Execute a command; reconnect once on broken connection.
void* RedisClient::ExecCommand(const char* fmt, ...) {
    if (!ctx_ && !Connect()) return nullptr;

    va_list args;
    va_start(args, fmt);
    void* reply = redisvCommand(ctx_, fmt, args);
    va_end(args);

    if (!reply && ctx_->err) {
        // Connection lost — try once to reconnect
        LOG_WARN("Redis command failed; reconnecting",
                 {{"error", ctx_->errstr}});
        if (Connect()) {
            va_start(args, fmt);
            reply = redisvCommand(ctx_, fmt, args);
            va_end(args);
        }
    }
    return reply;
}

bool RedisClient::SetNxPx(const std::string& key,
                           const std::string& value,
                           int64_t            ttl_ms) {
    OpTimer t("setnxpx");
    try {
        Reply r(redisCommand(ctx_ ? ctx_ : nullptr,
                             "SET %s %s NX PX %lld",
                             key.c_str(), value.c_str(),
                             static_cast<long long>(ttl_ms)));
        if (!r.ok() && !Connect()) return false;
        if (!r.ok()) {
            Reply r2(redisCommand(ctx_, "SET %s %s NX PX %lld",
                                  key.c_str(), value.c_str(),
                                  static_cast<long long>(ttl_ms)));
            return r2.is_status() && r2.ptr->str &&
                   std::string(r2.ptr->str) == "OK";
        }
        return r.is_status() && r.ptr->str &&
               std::string(r.ptr->str) == "OK";
    } catch (...) {
        LOG_WARN("Redis SetNxPx exception", {{"key", key}});
        return false;
    }
}

void RedisClient::Del(const std::string& key) {
    OpTimer t("del");
    try {
        if (!ctx_ && !Connect()) return;
        Reply r(redisCommand(ctx_, "DEL %s", key.c_str()));
        if (!r.ok() && ctx_->err) {
            LOG_WARN("Redis DEL failed", {{"key", key}, {"error", ctx_->errstr}});
        }
    } catch (...) {
        LOG_WARN("Redis Del exception", {{"key", key}});
    }
}

std::optional<std::string> RedisClient::Get(const std::string& key) {
    OpTimer t("get");
    try {
        if (!ctx_ && !Connect()) return std::nullopt;
        Reply r(redisCommand(ctx_, "GET %s", key.c_str()));
        if (!r.ok()) {
            if (ctx_ && ctx_->err) {
                LOG_WARN("Redis GET failed",
                         {{"key", key}, {"error", ctx_->errstr}});
            }
            return std::nullopt;
        }
        if (r.is_nil()) return std::nullopt;
        if (r.is_string()) return std::string(r.ptr->str, r.ptr->len);
        return std::nullopt;
    } catch (...) {
        LOG_WARN("Redis Get exception", {{"key", key}});
        return std::nullopt;
    }
}

void RedisClient::Set(const std::string& key,
                      const std::string& value,
                      int64_t            ttl_ms) {
    OpTimer t("set");
    try {
        if (!ctx_ && !Connect()) return;
        Reply r(redisCommand(ctx_, "SET %s %b PX %lld",
                             key.c_str(),
                             value.data(), value.size(),
                             static_cast<long long>(ttl_ms)));
        if (!r.ok() && ctx_ && ctx_->err) {
            LOG_WARN("Redis SET failed",
                     {{"key", key}, {"error", ctx_->errstr}});
        }
    } catch (...) {
        LOG_WARN("Redis Set exception", {{"key", key}});
    }
}

int64_t RedisClient::Incr(const std::string& key) {
    OpTimer t("incr");
    try {
        if (!ctx_ && !Connect()) return 0;
        Reply r(redisCommand(ctx_, "INCR %s", key.c_str()));
        if (!r.ok()) {
            LOG_WARN("Redis INCR failed", {{"key", key}});
            return 0;
        }
        if (r.is_integer()) return r.ptr->integer;
        return 0;
    } catch (...) {
        LOG_WARN("Redis Incr exception", {{"key", key}});
        return 0;
    }
}

void RedisClient::Expire(const std::string& key, int64_t ttl_ms) {
    OpTimer t("expire");
    try {
        if (!ctx_ && !Connect()) return;
        Reply r(redisCommand(ctx_, "PEXPIRE %s %lld",
                             key.c_str(),
                             static_cast<long long>(ttl_ms)));
        if (!r.ok() && ctx_ && ctx_->err) {
            LOG_WARN("Redis PEXPIRE failed",
                     {{"key", key}, {"error", ctx_->errstr}});
        }
    } catch (...) {
        LOG_WARN("Redis Expire exception", {{"key", key}});
    }
}

// ---------------------------------------------------------------------------
// DistributedLock
// ---------------------------------------------------------------------------

DistributedLock::DistributedLock(IRedisLockable&    client,
                                  const std::string& key,
                                  const std::string& owner_id,
                                  int64_t            ttl_ms)
    : client_(client)
    , lock_key_("lock:job:" + key) {
    acquired_ = client_.SetNxPx(lock_key_, owner_id, ttl_ms);
    if (!acquired_) {
        throw std::runtime_error("Could not acquire distributed lock for key: " + lock_key_);
    }
}

DistributedLock::~DistributedLock() {
    if (acquired_) {
        client_.Del(lock_key_);
    }
}

}  // namespace jq
