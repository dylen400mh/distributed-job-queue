# Activity Log

## 2026-02-21

### Prompt
> Yeah I'd like you to start with this: [Prompt 1 — Project Scaffold & Build System]
> Scaffold the complete project structure for the distributed job queue. Create directory layout, CMakeLists.txt, toolchain file, vcpkg.json, .gitignore, placeholder main.cc files, and verify build.

### Actions
1. Read tech-stack.md, design-notes.md, requirements.md, prompts.md
2. Wrote plan to tasks/todo.md
3. Created full directory layout: proto/, src/{server,worker,ctl,common}/, db/migrations/, k8s/, docker/, cmake/, prometheus/, grafana/, docs/, tests/{unit,integration,e2e}/
4. Installed Homebrew dependencies: grpc, protobuf, libpqxx, hiredis, librdkafka, boost, abseil, spdlog, nlohmann-json, googletest, yaml-cpp, prometheus-cpp, pkg-config
5. Created cmake/toolchain-macos.cmake — points to Homebrew LLVM clang/clang++ at /opt/homebrew/opt/llvm/bin/
6. Created CMakeLists.txt — C++17, three targets (jq-server, jq-worker, jq-ctl), find_package for all deps, tests/ subdirectory
   - Fixed Boost: Boost.System is header-only in Boost 1.90; use only Boost::filesystem + Boost::headers
   - Fixed pkg-config paths: extended PKG_CONFIG_PATH to include keg-only Homebrew opt/* paths for libpq, libpqxx, hiredis, librdkafka
7. Created vcpkg.json — all C++ dependencies listed for non-macOS fallback
8. Created .gitignore — standard C++/CMake ignores
9. Created placeholder main.cc files for jq-server, jq-worker, jq-ctl (each just returns 0)
10. Created tests/CMakeLists.txt — placeholder comments for future test targets
11. Verified build: cmake configure succeeded, all three binaries compiled cleanly with Homebrew LLVM Clang 21.1.8

---

### Prompt
> [Prompt 2 — Protobuf & gRPC Service Definitions]
> Create all Protocol Buffer service and message definitions in proto/. Update CMakeLists.txt to add protobuf/gRPC code generation.

### Actions
1. Verified protoc (libprotoc 33.4) and grpc_cpp_plugin at /opt/homebrew/bin/
2. Created proto/common.proto — JobStatus enum, WorkerStatus enum, Job/Queue/Worker/JobEvent messages using google.protobuf.Timestamp
3. Created proto/job_service.proto — JobService (6 RPCs): SubmitJob, CancelJob, GetJobStatus, ListJobs, GetJobLogs, RetryJob
4. Created proto/worker_service.proto — WorkerService (5 RPCs): RegisterWorker, Heartbeat, StreamJobs (server-streaming), ReportResult, Deregister
5. Created proto/admin_service.proto — AdminService (8 RPCs): CreateQueue, DeleteQueue, ListQueues, GetQueueStats, ListWorkers, DrainWorker, ShutdownWorker, GetSystemStatus
6. Fixed: removed unused import of common.proto from worker_service.proto (all WorkerService messages use only primitive types)
7. Verified all .proto files compile cleanly with protoc (no warnings or errors)
8. Updated CMakeLists.txt: added proto codegen via add_custom_command per .proto file; created proto_gen static library; added proto_gen to COMMON_LIBS
9. Verified full cmake configure + build: proto_gen library built (8 generated .cc files), all three binaries linked successfully
