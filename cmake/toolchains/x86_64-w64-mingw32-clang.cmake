# Cross-compiling for 64-bit Windows from Linux with Clang targeting MinGW-w64 (GNU ABI, UCRT), LLD as the linker.
# Uses the distribution's MinGW-w64 headers, CRT and GCC runtime (libstdc++, winpthreads) under /usr/x86_64-w64-mingw32.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CSX_MINGW_TRIPLE x86_64-w64-mingw32)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${CSX_MINGW_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${CSX_MINGW_TRIPLE})
set(CMAKE_RC_COMPILER llvm-windres)
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_DLLTOOL llvm-dlltool)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")

set(CMAKE_FIND_ROOT_PATH /usr/${CSX_MINGW_TRIPLE})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
