cmake_minimum_required(VERSION 3.14)
project(Assignment_11 VERSION 1.0 DESCRIPTION Group_Chat LANGUAGES C)
include_directories(include)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# SANITIZER can be set to "address", "thread", or "none" (default).
# ASan and TSan cannot be linked together, so pick one per build:
#   cmake -S . -B build-asan -DSANITIZER=address
#   cmake -S . -B build-tsan -DSANITIZER=thread
set(SANITIZER "none" CACHE STRING "Sanitizer to build with: none, address, thread")

add_compile_options(-Wall -Wextra -g -O0 -fno-omit-frame-pointer)
add_link_options(-pthread)

if(SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread)
  add_link_options(-fsanitize=thread)
elseif(NOT SANITIZER STREQUAL "none")
  message(FATAL_ERROR "Unknown SANITIZER value: ${SANITIZER} (expected none, address, or thread)")
endif()

add_executable(server src/server.c)
target_link_libraries(server pthread)

add_executable(client src/client.c src/hex_string.c)
target_link_libraries(client pthread)
