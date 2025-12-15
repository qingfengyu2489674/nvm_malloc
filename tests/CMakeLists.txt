# 查找 Threads 包 (Linux 下对应 pthread)
find_package(Threads REQUIRED)

# 1. 自动发现所有测试源文件
file(GLOB test_sources "*.c")

# 2. 为每个常规单元测试文件创建可执行文件
foreach(test_source ${test_sources})
    get_filename_component(test_name ${test_source} NAME_WE)

    # --- 特殊处理: 跳过多线程压力测试 ---
    # 我们将在循环外部单独配置它，因为它需要 pthread 且不使用 Unity
    if(${test_name} STREQUAL "test_nvm_multithread")
        continue()
    endif()

    add_executable(${test_name} ${test_source})

    # 3. 链接依赖库 (常规测试链接 Unity)
    target_link_libraries(${test_name} PRIVATE ${CMAKE_PROJECT_NAME} unity)

    # 为了能 #include "src/**.c" 进行白盒测试
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    # 4. 注册测试
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# ==============================================================================
# 5. 单独配置多线程压力测试 (test_nvm_multithread)
# ==============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test_nvm_multithread.c")
    add_executable(test_nvm_multithread test_nvm_multithread.c)

    # 链接核心库 + 线程库 (不链接 Unity)
    target_link_libraries(test_nvm_multithread PRIVATE
        ${CMAKE_PROJECT_NAME}
        Threads::Threads
    )

    # 允许访问内部头文件 (如果需要)
    target_include_directories(test_nvm_multithread PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    add_test(NAME test_nvm_multithread COMMAND test_nvm_multithread)
endif()