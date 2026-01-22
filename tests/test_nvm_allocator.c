#include "unity.h"

// 包含所有必要的头文件
#include "NvmDefs.h"
#include "NvmSlab.h"
#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmAllocator.h"

// 包含所有组件的实现文件
#include "NvmSlab.c"
#include "NvmSpaceManager.c"
#include "SlabHashTable.c"
#include "NvmAllocator.c"

#include <stdlib.h>
#include <string.h>
#include <sched.h> // 用于 CPU 绑定

#define MAX_BLOCK_SIZE 4096
#define TOTAL_NVM_SIZE (10 * NVM_SLAB_SIZE)
#define NUM_SLABS 10

static void* mock_nvm_base = NULL;
extern struct NvmAllocator* global_nvm_allocator;

// ... (setUp 和 tearDown 保持不变) ...
void setUp(void) {
    mock_nvm_base = malloc(TOTAL_NVM_SIZE);
    TEST_ASSERT_NOT_NULL(mock_nvm_base);
    memset(mock_nvm_base, 0, TOTAL_NVM_SIZE);
    int result = nvm_allocator_create(mock_nvm_base, TOTAL_NVM_SIZE);
    TEST_ASSERT_EQUAL_INT(0, result);
}

void tearDown(void) {
    nvm_allocator_destroy();
    free(mock_nvm_base);
    mock_nvm_base = NULL;
}

// ... (test_allocator_lifecycle 保持不变) ...
void test_allocator_lifecycle(void) {
    TEST_ASSERT_NOT_NULL(global_nvm_allocator);
    TEST_ASSERT_EQUAL_PTR(mock_nvm_base, global_nvm_allocator->central_heap.nvm_base_addr);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->central_heap.space_manager);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->central_heap.slab_lookup_table);
    TEST_ASSERT_EQUAL_UINT64(TOTAL_NVM_SIZE, global_nvm_allocator->central_heap.space_manager->head->size);
    for (int i = 0; i < SC_COUNT; ++i) {
        TEST_ASSERT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[i]);
    }

    nvm_allocator_destroy(); 
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_create(NULL, TOTAL_NVM_SIZE)); 
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_create(mock_nvm_base, NVM_SLAB_SIZE - 1)); 

    nvm_allocator_destroy(); 
    nvm_allocator_destroy(); 
}

// ... (test_basic_malloc_and_free 保持不变) ...
void test_basic_malloc_and_free(void) {
    void* ptr = nvm_malloc(30);
    TEST_ASSERT_NOT_NULL(ptr);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_32B]); // 这里的[0]现在安全了
    TEST_ASSERT_EQUAL_UINT32(1, global_nvm_allocator->central_heap.slab_lookup_table->count);
    TEST_ASSERT_EQUAL_UINT64((NUM_SLABS - 1) * NVM_SLAB_SIZE, global_nvm_allocator->central_heap.space_manager->head->size);

    nvm_free(ptr);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_32B]);
    TEST_ASSERT_TRUE(nvm_slab_is_empty(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_32B]));
}

// ... (test_slab_creation_and_reuse 保持不变) ...
void test_slab_creation_and_reuse(void) {
    void* ptr1 = nvm_malloc(60);
    TEST_ASSERT_NOT_NULL(ptr1);
    NvmSlab* slab64_ptr = global_nvm_allocator->cpu_heaps[0].slab_lists[SC_64B];
    TEST_ASSERT_NOT_NULL(slab64_ptr);

    void* ptr2 = nvm_malloc(60);
    TEST_ASSERT_NOT_NULL(ptr2);
    TEST_ASSERT_EQUAL_PTR(slab64_ptr, global_nvm_allocator->cpu_heaps[0].slab_lists[SC_64B]);

    void* ptr3 = nvm_malloc(8);
    TEST_ASSERT_NOT_NULL(ptr3);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_8B]);
    TEST_ASSERT_EQUAL_UINT32(2, global_nvm_allocator->central_heap.slab_lookup_table->count);
}

// ... (test_empty_slab_recycling 保持不变) ...
void test_empty_slab_recycling(void) {
    size_t alloc_size = 128;
    SizeClassID sc_id = SC_128B;
    uint32_t blocks_per_slab = NVM_SLAB_SIZE / alloc_size;

    void** ptrs = malloc(sizeof(void*) * (blocks_per_slab + 1));
    TEST_ASSERT_NOT_NULL(ptrs);
    
    for (uint32_t i = 0; i < blocks_per_slab; ++i) {
        ptrs[i] = nvm_malloc(alloc_size);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    
    ptrs[blocks_per_slab] = nvm_malloc(alloc_size);
    TEST_ASSERT_NOT_NULL(ptrs[blocks_per_slab]);
    
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[sc_id]->next_in_chain);
    
    for (uint32_t i = 0; i < blocks_per_slab; ++i) {
        nvm_free(ptrs[i]);
    }

    NvmSlab* first_slab = global_nvm_allocator->cpu_heaps[0].slab_lists[sc_id]; 
    TEST_ASSERT_NOT_NULL(first_slab); // 增加断言，防止NULL解引用
    
    NvmSlab* second_slab = first_slab->next_in_chain;                           
    
    TEST_ASSERT_NOT_NULL(second_slab); 
    TEST_ASSERT_TRUE(nvm_slab_is_empty(second_slab)); 
    
    TEST_ASSERT_EQUAL_UINT32(2, global_nvm_allocator->central_heap.slab_lookup_table->count);
    
    nvm_free(ptrs[blocks_per_slab]);
    free(ptrs);
}

// ... (test_parameter_and_error_handling, test_nvm_space_exhaustion, test_mixed_load_and_fragmentation 保持不变) ...
void test_parameter_and_error_handling(void) {
    TEST_ASSERT_NULL(nvm_malloc(0));
    TEST_ASSERT_NULL(nvm_malloc(MAX_BLOCK_SIZE + 1));
    nvm_free(NULL); 
}

void test_nvm_space_exhaustion(void) {
    nvm_allocator_destroy();
    const size_t small_nvm_size = 2 * NVM_SLAB_SIZE;
    TEST_ASSERT_EQUAL_INT(0, nvm_allocator_create(mock_nvm_base, small_nvm_size));

    for (int i = 0; i < NVM_SLAB_SIZE / 8; ++i) nvm_malloc(8);
    for (int i = 0; i < NVM_SLAB_SIZE / 16; ++i) nvm_malloc(16);

    TEST_ASSERT_NULL(global_nvm_allocator->central_heap.space_manager->head);
    TEST_ASSERT_NULL(nvm_malloc(32));
}

void test_mixed_load_and_fragmentation(void) {
    const int num_allocs = 1000;
    void** ptrs = malloc(sizeof(void*) * num_allocs);
    TEST_ASSERT_NOT_NULL(ptrs);

    for (int i = 0; i < num_allocs; ++i) {
        ptrs[i] = nvm_malloc((i % 100) + 1);
        TEST_ASSERT_NOT_NULL(ptrs[i]);
    }
    for (int i = 0; i < num_allocs; i += 2) {
        nvm_free(ptrs[i]);
        ptrs[i] = NULL;
    }
    for (int i = 0; i < num_allocs / 2; ++i) {
        TEST_ASSERT_NOT_NULL(nvm_malloc((i % 50) + 1));
    }
    
    free(ptrs);
}




void test_debug_print_api(void) {
    // Case 1: 正常初始化状态下的打印
    printf("\n>>> [TEST START] Visual Check for nvm_allocator_debug_print <<<\n");
    
    // 为了让打印内容有意义，我们先分配几个不同大小的内存，触发 Slab 的创建和哈希表插入
    void* p1 = nvm_malloc(32);   // 触发 SC_32B Slab
    void* p2 = nvm_malloc(4096); // 触发 SC_4K Slab
    void* p3 = nvm_malloc(32);   // 复用 SC_32B Slab

    // 调用被测接口
    // 预期输出：
    // 1. NVM Base Address (非空)
    // 2. Hash Table 容量和计数
    // 3. 至少两个 Bucket 的条目 (对应 32B 和 4KB 的 Slab)
    nvm_allocator_debug_print();

    // 清理分配
    nvm_free(p1);
    nvm_free(p2);
    nvm_free(p3);

    printf(">>> [TEST END] Visual Check for nvm_allocator_debug_print <<<\n");

    // Case 2: 未初始化状态下的打印
    // 先手动销毁分配器
    nvm_allocator_destroy();

    printf("\n>>> [TEST START] Visual Check for Uninitialized State <<<\n");
    // 调用接口
    // 预期输出：提示 "Allocator is not initialized" 或类似错误，且程序不应崩溃
    nvm_allocator_debug_print();
    printf(">>> [TEST END] Visual Check for Uninitialized State <<<\n");

    // 恢复现场：重新创建分配器，以便 tearDown 能够正常运行
    // (虽然 tearDown 里的 nvm_allocator_destroy 也能处理 NULL，但保持状态一致是个好习惯)
    int res = nvm_allocator_create(mock_nvm_base, TOTAL_NVM_SIZE);
    TEST_ASSERT_EQUAL_INT(0, res);
}


// ============================================================================
//                          新增：高压调试接口测试
// ============================================================================

void test_debug_print_high_pressure_verification(void) {
    printf("\n>>> [TEST START] High Pressure Accuracy Verification (Multi-Slab + Cache Churn) <<<\n");

    // ========================================================================
    // 1. 压力参数配置
    // ========================================================================
    // SC_4K (4096B): Slab 大小 2MB -> 容量 512 个
    // 分配 600 个 -> 必定触发 Slab #0 满，Slab #1 使用 88 个
    const int count_4k = 600;
    const size_t size_4k = 4096;
    
    // SC_64B (64B): 用于增加混淆
    const int count_64b = 100;
    const size_t size_64b = 64;

    void** ptrs_4k = (void**)malloc(sizeof(void*) * count_4k);
    void** ptrs_64b = (void**)malloc(sizeof(void*) * count_64b);
    TEST_ASSERT_NOT_NULL(ptrs_4k);
    TEST_ASSERT_NOT_NULL(ptrs_64b);

    // ========================================================================
    // 2. 批量分配 (填充阶段)
    // ========================================================================
    printf("\n[Step 1] Allocating %d objects of 4KB (Spanning 2 Slabs)...\n", count_4k);
    for (int i = 0; i < count_4k; i++) {
        ptrs_4k[i] = nvm_malloc(size_4k);
        TEST_ASSERT_NOT_NULL(ptrs_4k[i]);
    }

    printf("[Step 2] Allocating %d objects of 64B...\n", count_64b);
    for (int i = 0; i < count_64b; i++) {
        ptrs_64b[i] = nvm_malloc(size_64b);
        TEST_ASSERT_NOT_NULL(ptrs_64b[i]);
    }

    // ========================================================================
    // 3. 制造高压碎片 (触发 Cache Drain 和 Bitmap 混合状态)
    // ========================================================================
    // 策略：
    // - 4KB 对象：释放所有偶数索引 (0, 2, 4...) -> 释放 300 个
    //   这会导致 Cache (size=64) 填满 -> 回写位图 -> 填满 -> 回写...
    //   最终状态下，位图会有大量空洞，Cache 里会有残留的空闲块。
    //   DEBUG API 必须能正确识别出这些“已释放但还在 Cache 里”的块，不打印它们。
    
    printf("[Step 3] Heavy Fragmentation: Freeing 50%% of objects (Odds vs Evens)...\n");
    int active_4k = 0;
    for (int i = 0; i < count_4k; i++) {
        if (i % 2 == 0) {
            nvm_free(ptrs_4k[i]); // 释放偶数
            ptrs_4k[i] = NULL;
        } else {
            active_4k++;          // 保留奇数
        }
    }

    int active_64b = 0;
    for (int i = 0; i < count_64b; i++) {
        if (i % 3 == 0) {         // 释放索引能被 3 整除的
            nvm_free(ptrs_64b[i]);
            ptrs_64b[i] = NULL;
        } else {
            active_64b++;
        }
    }

    // ========================================================================
    // 4. 用户侧真值表 (Ground Truth)
    // ========================================================================
    printf("\n================ [USER SIDE GROUND TRUTH] ================\n");
    printf("Expected 4KB Objects: %d (Indices 1, 3, 5...)\n", active_4k);
    printf("Expected 64B Objects: %d\n", active_64b);
    printf("Total Objects: %d\n", active_4k + active_64b);
    printf("----------------------------------------------------------\n");
    printf("List of currently held pointers (Partial/Full Dump):\n");
    
    // 打印所有 4KB 对象（这是验证重点，因为它跨 Slab）
    // 为了防止刷屏太快，我们只打印前 10 个和最后 10 个，
    // 或者如果你想完全对比，可以取消注释全部打印。
    // 这里为了响应你的要求“输出每个分配的块”，我们全部打印。
    
    int print_count = 0;
    for (int i = 0; i < count_4k; i++) {
        if (ptrs_4k[i] != NULL) {
            printf("  [User 4K #%03d] %p\n", i, ptrs_4k[i]);
            print_count++;
        }
    }
    for (int i = 0; i < count_64b; i++) {
        if (ptrs_64b[i] != NULL) {
            printf("  [User 64B #%03d] %p\n", i, ptrs_64b[i]);
        }
    }
    printf("==========================================================\n");

    // ========================================================================
    // 5. 系统侧 Debug 输出
    // ========================================================================
    printf("\nvvvvvvvvvvvvvvvv [SYSTEM DEBUG OUTPUT] vvvvvvvvvvvvvvvvvvv\n");
    // 预期观察：
    // 1. 应该有 2 个 4KB Slab。
    //    - Slab #0 (Offset 0): 总量 512，使用 256 (因为释放了一半)
    //    - Slab #1 (Offset 2MB): 总量 512，使用 44 (88的一半)
    // 2. 地址必须与上方 [USER SIDE] 完全一致。
    // 3. 不应出现任何 NULL 或已释放的偶数索引地址。
    nvm_allocator_debug_print();
    printf("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^\n");

    // ========================================================================
    // 6. 清理
    // ========================================================================
    printf("\n[Step 5] Cleaning up remaining objects...\n");
    for (int i = 0; i < count_4k; i++) {
        if (ptrs_4k[i]) nvm_free(ptrs_4k[i]);
    }
    for (int i = 0; i < count_64b; i++) {
        if (ptrs_64b[i]) nvm_free(ptrs_64b[i]);
    }
    free(ptrs_4k);
    free(ptrs_64b);

    printf(">>> [TEST END] High Pressure Verification Complete <<<\n");
}

// ============================================================================
//                          测试执行入口 (关键修改)
// ============================================================================
int main(void) {
    // --- 强制绑定到 CPU 0 ---
    // 这保证了 sched_getcpu() 始终返回 0，从而使测试代码中
    // 硬编码的 cpu_heaps[0] 假设成立。
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity failed");
        return -1;
    }
    // ----------------------

    UNITY_BEGIN();

    RUN_TEST(test_allocator_lifecycle);
    RUN_TEST(test_basic_malloc_and_free);
    RUN_TEST(test_slab_creation_and_reuse);
    RUN_TEST(test_empty_slab_recycling);
    RUN_TEST(test_parameter_and_error_handling);
    RUN_TEST(test_nvm_space_exhaustion);
    RUN_TEST(test_mixed_load_and_fragmentation);

    RUN_TEST(test_debug_print_api);

    RUN_TEST(test_debug_print_high_pressure_verification);

    return UNITY_END();
}