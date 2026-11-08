# JCE A1 x86 XP SP3 probe-only toolchain.
set(JCE_XP_TRIPLE i686-w64-mingw32)
set(JCE_XP_PE_MINOR 1)
set(JCE_XP_WINNT 0x0501)
set(CMAKE_SYSTEM_PROCESSOR i686)
include("${CMAKE_CURRENT_LIST_DIR}/windows-xp-llvm-common.cmake")
