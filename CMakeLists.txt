cmake_minimum_required(VERSION 3.16)
project(log_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_library(le INTERFACE)
target_include_directories(le INTERFACE include)
target_link_libraries(le INTERFACE Threads::Threads)
if(MSVC)
  target_compile_options(le INTERFACE /W4 /O2)
else()
  target_compile_options(le INTERFACE -Wall -Wextra -O2)
endif()

if(MINGW)
  # без этого exe не находит libstdc++/libwinpthread DLL вне окружения MinGW
  target_link_options(le INTERFACE -static)
endif()

add_executable(logengine src/main.cpp)
target_link_libraries(logengine PRIVATE le)

add_executable(gen_logs tools/gen_logs.cpp)
target_link_libraries(gen_logs PRIVATE le)
