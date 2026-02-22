#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <stdexcept>
#include <string>

#include "common/redis/redis_client.h"

// ---------------------------------------------------------------------------
// Mock implementation of IRedisLockable
// ---------------------------------------------------------------------------
class MockRedis : public jq::IRedisLockable {
public:
    MOCK_METHOD(bool, SetNxPx,
                (const std::string& key, const std::string& value, int64_t ttl_ms),
                (override));
    MOCK_METHOD(void, Del, (const std::string& key), (override));
};

using ::testing::_;
using ::testing::Return;
using ::testing::InSequence;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// When SetNxPx returns true the lock is acquired and Del is called on destruct.
TEST(DistributedLockTest, AcquireAndRelease) {
    MockRedis redis;

    InSequence seq;
    EXPECT_CALL(redis, SetNxPx("lock:job:job-123", "owner-1", 60000))
        .WillOnce(Return(true));
    EXPECT_CALL(redis, Del("lock:job:job-123"))
        .Times(1);

    {
        jq::DistributedLock lock(redis, "job-123", "owner-1", 60000);
        EXPECT_EQ(lock.LockKey(), "lock:job:job-123");
    }  // destructor fires Del here
}

// When SetNxPx returns false the constructor throws and Del is NOT called.
TEST(DistributedLockTest, ThrowsWhenLockNotAvailable) {
    MockRedis redis;

    EXPECT_CALL(redis, SetNxPx(_, _, _)).WillOnce(Return(false));
    EXPECT_CALL(redis, Del(_)).Times(0);

    EXPECT_THROW(
        { jq::DistributedLock lock(redis, "job-456", "owner-2"); },
        std::runtime_error);
}

// LockKey is prefixed with "lock:job:".
TEST(DistributedLockTest, LockKeyIsCorrectlyPrefixed) {
    MockRedis redis;
    EXPECT_CALL(redis, SetNxPx("lock:job:my-job", _, _)).WillOnce(Return(true));
    EXPECT_CALL(redis, Del(_)).Times(1);

    jq::DistributedLock lock(redis, "my-job", "owner");
    EXPECT_EQ(lock.LockKey(), "lock:job:my-job");
}

// Del is called exactly once even if the lock object goes out of scope early.
TEST(DistributedLockTest, DelCalledExactlyOnce) {
    MockRedis redis;
    EXPECT_CALL(redis, SetNxPx(_, _, _)).WillOnce(Return(true));
    EXPECT_CALL(redis, Del(_)).Times(1);

    auto* lock = new jq::DistributedLock(redis, "job-789", "owner");
    delete lock;
}

// Default TTL is 60 000 ms.
TEST(DistributedLockTest, DefaultTtlIs60Seconds) {
    MockRedis redis;
    EXPECT_CALL(redis, SetNxPx(_, _, 60000)).WillOnce(Return(true));
    EXPECT_CALL(redis, Del(_)).Times(1);

    jq::DistributedLock lock(redis, "job-default-ttl", "owner");
}
