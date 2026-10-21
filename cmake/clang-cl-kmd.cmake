# Cross toolchain for the kernel driver (x86_64): clang-cl and lld-link from the dev shell, WDK kernel headers and
# libraries from refs/winkits. A kernel driver links no C runtime, so no MSVC installation is involved.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER "$ENV{VGD_CLANG_CL}")
set(CMAKE_CXX_COMPILER "$ENV{VGD_CLANG_CL}")
set(CMAKE_RC_COMPILER "$ENV{VGD_LLVM_RC}")
set(CMAKE_LINKER "$ENV{VGD_LLD_LINK}")
set(CMAKE_AR "$ENV{VGD_LLVM_LIB}")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# Compiler checks would link a user-mode program against a CRT we do not have.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
