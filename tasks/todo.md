# Prompt 2 — Protobuf & gRPC Service Definitions

## Todo

- [x] 1. Create proto/common.proto — JobStatus enum, WorkerStatus enum, Job/Queue/Worker messages (using google.protobuf.Timestamp for time fields)
- [x] 2. Create proto/job_service.proto — JobService with 6 RPCs + all request/response messages
- [x] 3. Create proto/worker_service.proto — WorkerService with 5 RPCs + all request/response messages (StreamJobs is server-streaming)
- [x] 4. Create proto/admin_service.proto — AdminService with 8 RPCs + all request/response messages
- [x] 5. Update CMakeLists.txt — add protoc + grpc_cpp_plugin code generation step; create a proto_gen static library; link it to all three binaries
- [x] 6. Verify: cmake configure + build succeeds, all .proto files compile without errors
- [x] 7. Append to docs/activity.md and push to git

## Review

All tasks completed successfully. Proto definitions are in place and wired into the build.

**What was created:**
- `proto/common.proto` — `JobStatus`/`WorkerStatus` enums; `Job`, `Queue`, `Worker`, `JobEvent` messages
- `proto/job_service.proto` — `JobService` with 6 RPCs and all request/response types
- `proto/worker_service.proto` — `WorkerService` with 5 RPCs (StreamJobs is server-streaming)
- `proto/admin_service.proto` — `AdminService` with 8 RPCs including QueueStats and ComponentStatus messages

**CMakeLists.txt changes:**
- `add_custom_command` per `.proto` file runs protoc + grpc_cpp_plugin, generating `.pb.cc`/`.pb.h` and `.grpc.pb.cc`/`.grpc.pb.h`
- `proto_gen` static library compiles all 8 generated `.cc` files; prepended to `COMMON_LIBS`

**Fix:** Removed unused `import "common.proto"` from `worker_service.proto` (all messages use only primitive types).

**Build result:** 4 .proto files generate cleanly; `proto_gen` built from 8 generated source files; all three binaries linked successfully.
