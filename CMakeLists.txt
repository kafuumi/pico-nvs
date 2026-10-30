cmake_minimum_required(VERSION 3.16)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_STANDARD 11)

project(pico-nvs C)

add_executable(${PROJECT_NAME}
        src/nvs.c)

target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)

target_include_directories(${PROJECT_NAME} PUBLIC "include/")
target_include_directories(${PROJECT_NAME} PRIVATE "src/")
