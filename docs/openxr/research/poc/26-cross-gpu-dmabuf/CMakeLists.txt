cmake_minimum_required(VERSION 3.20)
project(xgpu_dmabuf CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

add_executable(xgpu_dmabuf xgpu_dmabuf.cpp)
target_link_libraries(xgpu_dmabuf PRIVATE Vulkan::Vulkan)
target_compile_options(xgpu_dmabuf PRIVATE -Wall -Wextra -Wno-unused-parameter -g -O2)
