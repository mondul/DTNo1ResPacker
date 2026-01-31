cmake_minimum_required(VERSION 3.20)
project(DTNo1ResPacker LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# nlohmann/json (header-only)
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# LZ4: fetch sources, build our own target (avoids upstream target-name variance)
FetchContent_Declare(
  lz4_src
  GIT_REPOSITORY https://github.com/lz4/lz4.git
  GIT_TAG v1.9.4
)
FetchContent_GetProperties(lz4_src)
if(NOT lz4_src_POPULATED)
  FetchContent_Populate(lz4_src)
endif()

add_library(lz4_local STATIC
  ${lz4_src_SOURCE_DIR}/lib/lz4.c
  ${lz4_src_SOURCE_DIR}/lib/lz4hc.c
)
target_include_directories(lz4_local PUBLIC
  ${lz4_src_SOURCE_DIR}/lib
)

add_executable(DTNo1ResPacker
  src/main.cpp
)

target_include_directories(DTNo1ResPacker PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
)

target_link_libraries(DTNo1ResPacker PRIVATE
  nlohmann_json::nlohmann_json
  lz4_local
)

if(MSVC)
  target_compile_options(DTNo1ResPacker PRIVATE /W4)
else()
  target_compile_options(DTNo1ResPacker PRIVATE -Wall -Wextra -Wpedantic)
endif()
