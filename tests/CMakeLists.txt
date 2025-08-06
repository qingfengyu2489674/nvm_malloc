# tests/CMakeLists.txt

# 1. 自动发现所有测试源文件
file(GLOB test_sources "*.c")

# 2. 为每个测试文件创建可执行文件和 CTest 测试
foreach(test_source ${test_sources})
    get_filename_component(test_name ${test_source} NAME_WE)

    add_executable(${test_name} ${test_source})

    # 3. 链接依赖库
    # 使用 ${CMAKE_PROJECT_NAME} 来链接主代码库，确保一致性
    target_link_libraries(${test_name} PRIVATE ${CMAKE_PROJECT_NAME} unity)

    # 为了能 #include "src/**.c" 进行白盒测试，我们需要手动添加 src 目录
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    # 4. 将可执行文件注册为一个 CTest 测试
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()