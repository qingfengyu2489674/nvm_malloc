# c-project-template/CMakeLists.txt

# 1. 最低版本要求和项目定义
#------------------------------------------------
cmake_minimum_required(VERSION 3.15)
project(my_project VERSION 1.0.0 LANGUAGES C)

# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -g")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")

# set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread -g -O2")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=thread")

# 开启 GNU 扩展，确定 sched_getcpu 可用
add_definitions(-D_GNU_SOURCE)

# 2. 全局设置
#------------------------------------------------
# 设置 C 标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 将 build/compile_commands.json 文件生成，便于IDE和插件使用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 设置所有目标的输出目录，保持根目录整洁
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)


# 3. 添加子目录
#------------------------------------------------
# 添加源码目录，这将创建一个库
add_subdirectory(src)

# 添加第三方库
# Unity 已经支持 CMake，可以直接添加
add_subdirectory(lib/Unity)


# 4. 启用和配置测试 (CTest)
#------------------------------------------------
# 只有在非 Release 模式下才构建测试
# 你也可以去掉这个 if 判断，总是构建测试
if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    enable_testing()
    add_subdirectory(tests)
endif()