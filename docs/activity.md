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
