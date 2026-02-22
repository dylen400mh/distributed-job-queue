#include "server/grpc/server.h"

#include <chrono>
#include <string>

#include <grpcpp/grpcpp.h>
#include <grpcpp/security/server_credentials.h>

#include "common/logging/logger.h"

namespace jq {

GrpcServer::GrpcServer(const Config&       cfg,
                        db::ConnectionPool& pool,
                        IKafkaProducer&     kafka)
    : cfg_(cfg)
    , job_repo_(pool)
    , job_svc_(job_repo_, kafka)
{}

void GrpcServer::Start() {
    const std::string addr = "0.0.0.0:" + std::to_string(cfg_.grpc.port);

    grpc::ServerBuilder builder;

    // TLS credentials (mTLS in production; insecure in local dev).
    if (cfg_.grpc.tls.enabled) {
        grpc::SslServerCredentialsOptions ssl_opts;
        grpc::SslServerCredentialsOptions::PemKeyCertPair kp;
        // In production, cert/key are read from files; simplified here.
        ssl_opts.pem_key_cert_pairs.push_back(kp);
        builder.AddListeningPort(addr, grpc::SslServerCredentials(ssl_opts));
    } else {
        builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    }

    builder.RegisterService(&job_svc_);
    builder.RegisterService(&worker_svc_);
    builder.RegisterService(&admin_svc_);

    server_ = builder.BuildAndStart();
    if (!server_) {
        throw std::runtime_error("Failed to start gRPC server on " + addr);
    }

    LOG_INFO("gRPC server listening", {{"addr", addr}});
    server_->Wait();  // blocks until Stop() is called
}

void GrpcServer::Stop() {
    if (!server_) return;
    LOG_INFO("gRPC server shutting down");
    // Give in-flight RPCs up to 30 s to complete (design-notes.md §Graceful Shutdown).
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(30);
    server_->Shutdown(deadline);
}

}  // namespace jq
