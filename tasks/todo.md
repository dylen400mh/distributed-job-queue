# Prompt 1 — Project Scaffold & Build System

## Todo

- [x] 1. Create full directory layout (proto/, src/server/, src/worker/, src/ctl/, src/common/, db/migrations/, k8s/, docker/, cmake/, prometheus/, grafana/, docs/, tests/)
- [x] 2. Create root CMakeLists.txt (C++17, three targets: jq-server/jq-worker/jq-ctl, link all deps, add tests/ subdirectory)
- [x] 3. Create cmake/toolchain-macos.cmake (Homebrew LLVM clang/clang++ at /opt/homebrew/opt/llvm/bin/)
- [x] 4. Create vcpkg.json (all C++ dependencies for non-macOS fallback)
- [x] 5. Create .gitignore (C++/CMake appropriate)
- [x] 6. Create placeholder main.cc for jq-server (src/server/main.cc)
- [x] 7. Create placeholder main.cc for jq-worker (src/worker/main.cc)
- [x] 8. Create placeholder main.cc for jq-ctl (src/ctl/main.cc)
- [x] 9. Verify build: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos.cmake -B build && cmake --build build
- [x] 10. Document activity in docs/activity.md
- [x] 11. Push changes to git

## Review

All tasks completed successfully. The build skeleton is in place.

**What was created:**
- Full directory layout matching tech-stack.md
- `CMakeLists.txt` — C++17, Homebrew LLVM toolchain, three targets (`jq-server`, `jq-worker`, `jq-ctl`), all dependencies wired via `find_package` and `pkg_check_modules`, `tests/` subdirectory included
- `cmake/toolchain-macos.cmake` — Homebrew LLVM clang 21.1.8
- `vcpkg.json` — all C++ deps for cross-platform fallback
- `.gitignore` — C++/CMake appropriate
- `src/{server,worker,ctl}/main.cc` — placeholder `int main()` stubs
- `tests/CMakeLists.txt` — placeholder for future test targets
- `docs/activity.md` — activity log started

**Notable fixes during implementation:**
- Boost 1.90 on Homebrew is header-only for `boost_system`; changed to `Boost::filesystem + Boost::headers`
- `libpq`, `libpqxx`, `hiredis`, and `librdkafka` are keg-only Homebrew formulas; their pkgconfig paths are non-standard and must be explicitly added to `PKG_CONFIG_PATH` in CMakeLists.txt

**Build result:** All three binaries compiled cleanly with Homebrew LLVM Clang 21.1.8.
