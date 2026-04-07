# write_sha256.cmake
# Invoked by cmake -P with -DFILE=<path> -DOUT=<output_path>
# Writes the lowercase SHA-256 hash of FILE into OUT (one line, no filename).
# Cross-platform: uses CMake built-in file(SHA256 ...) — no shell required.

if(NOT DEFINED FILE)
    message(FATAL_ERROR "write_sha256.cmake: -DFILE=<path> required")
endif()
if(NOT DEFINED OUT)
    message(FATAL_ERROR "write_sha256.cmake: -DOUT=<path> required")
endif()

file(SHA256 "${FILE}" _hash)
string(TOLOWER "${_hash}" _hash)
file(WRITE "${OUT}" "${_hash}\n")
