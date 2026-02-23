# cmake/toolchain-macos.cmake
# Homebrew LLVM toolchain for macOS development.
# Uses Homebrew-managed Clang instead of Apple's system Clang to ensure
# full C++17/20 support and compatibility with gRPC and Abseil.

set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

# Tell CMake to find Homebrew-installed packages first
list(PREPEND CMAKE_PREFIX_PATH /opt/homebrew)

# Use Homebrew LLVM's libc++ so that ABI symbols like __hash_memory are found.
# Homebrew clang compiles objects against its own libc++ headers; the system
# libc++ (MacOSX SDK) lacks newer ABI functions, causing linker errors.
set(CMAKE_EXE_LINKER_FLAGS
    "-L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++"
    CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS
    "-L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++"
    CACHE STRING "" FORCE)
