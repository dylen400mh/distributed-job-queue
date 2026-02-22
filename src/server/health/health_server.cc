#include "server/health/health_server.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

// POSIX socket headers
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pqxx/pqxx>

#include "common/logging/logger.h"

namespace jq {

namespace {

constexpr const char* k200 =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\nok";

constexpr const char* k503 =
    "HTTP/1.1 503 Service Unavailable\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 13\r\n"
    "Connection: close\r\n"
    "\r\nunhealthy\r\n\r\n";

constexpr const char* k404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 9\r\n"
    "Connection: close\r\n"
    "\r\nnot found";

// Read a line (up to newline) from a connected socket.
std::string ReadLine(int fd) {
    std::string line;
    char c;
    while (::read(fd, &c, 1) == 1) {
        if (c == '\n') break;
        if (c != '\r') line += c;
    }
    return line;
}

// Send a response and close the connection.
void SendResponse(int fd, const char* response) {
    ::write(fd, response, ::strlen(response));
    ::close(fd);
}

}  // namespace

// ---------------------------------------------------------------------------

HealthServer::HealthServer(int                 port,
                            db::ConnectionPool& pool,
                            RedisClient&        redis,
                            IKafkaProducer&     kafka)
    : port_(port), pool_(pool), redis_(redis), kafka_(kafka) {}

HealthServer::~HealthServer() {
    Stop();
}

void HealthServer::Start() {
    // Create the listening socket before spawning the thread so that the
    // caller can detect bind failures immediately.
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        throw std::runtime_error("health server: socket() failed");
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        throw std::runtime_error("health server: bind() failed on port " +
                                  std::to_string(port_));
    }
    ::listen(listen_fd_, 16);

    running_ = true;
    thread_  = std::thread(&HealthServer::AcceptLoop, this);
    LOG_INFO("Health server listening", {{"port", port_}});
}

void HealthServer::Stop() {
    if (!running_.exchange(false)) return;
    // Close the listening socket to unblock accept().
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (thread_.joinable()) thread_.join();
}

void HealthServer::AcceptLoop() {
    while (running_) {
        int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) break;  // listen_fd_ closed by Stop()

        // Read the request line, e.g. "GET /healthz HTTP/1.1"
        std::string request_line = ReadLine(client);

        // Drain remaining headers
        for (;;) {
            std::string h = ReadLine(client);
            if (h.empty()) break;
        }

        if (request_line.find("/readyz") != std::string::npos) {
            bool healthy = CheckDb() && CheckRedis();
            SendResponse(client, healthy ? k200 : k503);
        } else if (request_line.find("/healthz") != std::string::npos) {
            SendResponse(client, k200);
        } else {
            SendResponse(client, k404);
        }
    }
}

bool HealthServer::CheckDb() {
    try {
        auto c = pool_.Borrow();
        pqxx::work txn(c.get());
        txn.exec("SELECT 1");
        txn.commit();
        return true;
    } catch (...) {
        return false;
    }
}

bool HealthServer::CheckRedis() {
    // A successful Set returns without throwing; unavailability is caught
    // internally by RedisClient and returns false from SetNxPx.
    return redis_.IsConnected();
}

}  // namespace jq
