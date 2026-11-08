# JCE A1 x64 XP SP2 probe-only toolchain.
set(JCE_XP_TRIPLE x86_64-w64-mingw32)
set(JCE_XP_PE_MINOR 2)
set(JCE_XP_WINNT 0x0502)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
include("${CMAKE_CURRENT_LIST_DIR}/windows-xp-llvm-common.cmake")
