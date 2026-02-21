# cmake/toolchain-macos.cmake
# Homebrew LLVM toolchain for macOS development.
# Uses Homebrew-managed Clang instead of Apple's system Clang to ensure
# full C++17/20 support and compatibility with gRPC and Abseil.

set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

# Tell CMake to find Homebrew-installed packages first
list(PREPEND CMAKE_PREFIX_PATH /opt/homebrew)
