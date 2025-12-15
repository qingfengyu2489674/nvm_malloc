# src/CMakeLists.txt

# 1. 自动发现当前目录下的所有 .c 源文件
#------------------------------------------------
# GLOB_RECURSE 会递归查找，但这里我们用 GLOB 就够了，只查找当前目录。
# SRCS 变量将包含一个列表，例如 "template.c; another_file.c"
file(GLOB SRCS "*.c")


# 2. 定义一个库
#------------------------------------------------
# 检查是否找到了源文件，避免在目录为空时报错
if(NOT SRCS)
    message(WARNING "No source files found in ${CMAKE_CURRENT_SOURCE_DIR}. Library will be empty.")
else()
    # 使用项目名作为库名，这是一个好习惯。
    # CMAKE_PROJECT_NAME 是由根 CMakeLists.txt 的 project() 命令定义的。
    # ${SRCS} 会被展开为所有找到的源文件。
    add_library(${CMAKE_PROJECT_NAME} ${SRCS})

    # 3. 定义库的“接口”
    #------------------------------------------------
    # target_include_directories() 的目标名也使用变量
    target_include_directories(${CMAKE_PROJECT_NAME}
        PUBLIC
            # 对外暴露的公共头文件目录
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include> # 对应你的 src/include/template.h
            # 如果你采用我之前推荐的结构，应该是下面这行：
            # $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include> 
        PRIVATE
            # 内部私有头文件目录 (如果存在)
            # ${CMAKE_CURRENT_SOURCE_DIR}
    )
    
    message(STATUS "Library '${CMAKE_PROJECT_NAME}' created with sources: ${SRCS}")
endif()


