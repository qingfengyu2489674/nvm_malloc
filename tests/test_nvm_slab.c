#include "unity.h"
#include "nvm_slab.h"
#include "nvm_slab.c"
#include <stdlib.h>

#define SIMULATED_NVM_SIZE (2 * 1024 * 1024) // 2MB
static void* g_simulated_nvm_pool = NULL;    // 指向我们模拟的NVM空间的指针


// 在每个测试用例运行前，准备好模拟的 NVM 内存池
void setUp(void) {
    g_simulated_nvm_pool = malloc(SIMULATED_NVM_SIZE);
    TEST_ASSERT_NOT_NULL_MESSAGE(g_simulated_nvm_pool, "SETUP FAILED: Could not allocate simulated NVM pool.");
}

// 在每个测试用例运行后，清理模拟的 NVM 内存池
void tearDown(void) {
    free(g_simulated_nvm_pool);
    g_simulated_nvm_pool = NULL;
}

// ============================================================================
// 测试用例
// ============================================================================

/**
 * @brief 测试 nvm_slab_create 和 nvm_slab_destroy 的完整生命周期。
 * 
 * 本测试验证:
 * 1. nvm_slab_create 能否成功创建一个新的 Slab 元数据对象。
 * 2. 创建出的对象的内部字段（静态配置和动态状态）是否符合预期。
 * 3. nvm_slab_destroy 能否安全地被调用。
 * 4. 调用 create 和 destroy 后，不会导致内存泄漏（由测试框架和工具如 Valgrind 保证）。
 * 5. 对无效参数的鲁棒性。
 */
void test_nvm_slab_creation_and_destruction(void) {
    
    // --- 子测试 1: 测试一个典型、有效的 Slab 创建 (SC_256B) ---
    
    // Arrange: 准备参数
    const SizeClassID valid_sc_id = SC_256B;
    const uint64_t nvm_offset = 0;
    NvmSlab* slab = NULL; // 初始化为 NULL 是个好习惯

    // Act: 调用被测函数
    slab = nvm_slab_create(valid_sc_id, nvm_offset);

    // Assert: 验证结果
    TEST_ASSERT_NOT_NULL_MESSAGE(slab, "nvm_slab_create should succeed for a valid size class.");
    
    // 进行深入的“白盒”检查，验证内部状态是否正确
    // 检查静态配置
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(valid_sc_id, slab->size_type_id, "Size class ID mismatch.");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(nvm_offset, slab->nvm_base_offset, "NVM base offset mismatch.");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(256, slab->block_size, "Block size should be 256 for SC_256B.");
    // 理论计算: (2 * 1024 * 1024) / 256 = 8192
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(8192, slab->total_block_count, "Total block count mismatch.");

    // 检查初始动态状态 (calloc 应该已将它们清零)
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, slab->allocated_block_count, "Initial allocated_block_count should be 0.");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, slab->cache_count, "Initial cache_count should be 0.");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, slab->cache_head, "Initial cache_head should be 0.");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, slab->cache_tail, "Initial cache_tail should be 0.");
    TEST_ASSERT_NULL_MESSAGE(slab->next_in_chain, "Initial next_in_chain should be NULL.");

    // --- 子测试 2: 测试销毁逻辑 ---
    
    // Act & Assert: 调用销毁函数。这个调用不应该导致任何崩溃。
    // 我们无法直接验证内存是否被释放，但可以确保函数能被安全调用。
    // 后续使用 Valgrind 等内存检查工具可以确认无内存泄漏。
    nvm_slab_destroy(slab);
    // 销毁后，将本地指针设为 NULL 是一个良好的编程习惯
    slab = NULL;


    // --- 子测试 3: 测试对无效参数的处理 ---
    
    // Arrange: 准备一个无效的大小类 ID。
    // 我们假设 SC_COUNT 是枚举的最后一个成员，因此它本身是一个无效的ID。
    const SizeClassID invalid_sc_id = SC_COUNT;

    // Act: 使用无效ID调用创建函数
    slab = nvm_slab_create(invalid_sc_id, 12345);

    // Assert: 验证函数是否如预期一样返回 NULL
    TEST_ASSERT_NULL_MESSAGE(slab, "nvm_slab_create should return NULL for an invalid size class ID.");

    // --- 子测试 4: 测试销毁一个 NULL 指针的安全性 ---
    
    // Arrange: slab 已经是 NULL
    
    // Act & Assert: 调用销毁函数。这不应该导致任何问题，因为 free(NULL) 是安全操作。
    nvm_slab_destroy(slab);
}


// 这是修改后的、正确的测试函数

/**
 * @brief 对 alloc/free 的缓存机制进行详尽的白盒测试。
 *
 * 本测试深入检查 alloc 和 free 如何与 DRAM 缓存以及位图交互，验证
 * refill_cache 和 drain_cache 的触发时机和行为是否正确。
 */
void test_slab_alloc_free_cache_behavior(void) {
    // Arrange: 创建一个用于测试的Slab实例
    NvmSlab* slab = nvm_slab_create(SC_64B, 0);
    TEST_ASSERT_NOT_NULL(slab);
    
    // 获取一些常量，方便后续使用
    const uint32_t total_blocks = slab->total_block_count; // 2MB / 64B = 32768
    const uint32_t cache_size = SLAB_CACHE_SIZE; // 64
    const uint32_t batch_size = SLAB_CACHE_BATCH_SIZE; // 32
    uint32_t block_idx;
    int ret;
    // 用于存储分配的块索引的数组，以确保我们释放的是我们拥有的块
    uint32_t* allocated_indices = malloc(sizeof(uint32_t) * total_blocks);
    TEST_ASSERT_NOT_NULL(allocated_indices);

    // --- 子测试 1: 首次分配触发 refill_cache ---
    ret = nvm_slab_alloc(slab, &allocated_indices[0]);
    TEST_ASSERT_EQUAL_INT(0, ret);
    TEST_ASSERT_EQUAL_UINT32(1, slab->allocated_block_count);
    TEST_ASSERT_EQUAL_UINT32(batch_size - 1, slab->cache_count);

    // --- 子测试 2: 耗尽第一批缓存 (快速路径测试) ---
    for (int i = 1; i < batch_size; ++i) {
        ret = nvm_slab_alloc(slab, &allocated_indices[i]);
        TEST_ASSERT_EQUAL_INT(0, ret);
    }
    TEST_ASSERT_EQUAL_UINT32(0, slab->cache_count);
    TEST_ASSERT_EQUAL_UINT32(batch_size, slab->allocated_block_count);

    // --- 子测试 3: 填满缓存并触发 drain_cache ---
    // 先分配足够的块，以便后续可以释放它们
    for (int i = batch_size; i < cache_size; ++i) {
        ret = nvm_slab_alloc(slab, &allocated_indices[i]);
        TEST_ASSERT_EQUAL_INT(0, ret);
    }
    TEST_ASSERT_EQUAL_UINT32(cache_size, slab->allocated_block_count);
    
    // Act: 释放我们刚刚分配的 64 个块，这将填满缓存
    for (uint32_t i = 0; i < cache_size; ++i) {
        nvm_slab_free(slab, allocated_indices[i]);
    }

    // Assert: 缓存现在应该是满的
    TEST_ASSERT_EQUAL_UINT32(0, slab->allocated_block_count);
    TEST_ASSERT_EQUAL_UINT32(cache_size, slab->cache_count);

    // Act: 再释放一个我们拥有的块（例如，再次释放第0个块），这将触发 drain_cache
    // 注意：这在技术上是“双重释放”，但可以用来测试 drain 逻辑。
    // 一个更好的方法是分配第65个块，然后再释放它。
    ret = nvm_slab_alloc(slab, &allocated_indices[cache_size]); // 分配第 65 个块
    TEST_ASSERT_EQUAL_INT(0, ret); // allocated: 1, cache: 63
    
    nvm_slab_free(slab, allocated_indices[0]); // 释放一个已分配块，cache: 64, allocated: 0
    nvm_slab_free(slab, allocated_indices[cache_size]); // 释放第 65 个块，触发 drain

    // Assert:
    // 调用 free(allocated_indices[cache_size]) 时:
    // 1. cache_count 是 64，满了。
    // 2. drain_cache 被调用，将 cache_count 从 64 降到 32。
    // 3. 新释放的块被放入缓存，cache_count 变为 33。
    TEST_ASSERT_EQUAL_UINT32(batch_size + 1, slab->cache_count);

    // --- 子测试 4: 将 Slab 完全耗尽 ---
    // 首先清空状态，重新创建一个 slab 以进行干净的测试
    nvm_slab_destroy(slab);
    free(allocated_indices); // 释放旧的索引数组
    slab = nvm_slab_create(SC_64B, 0);
    TEST_ASSERT_NOT_NULL(slab);
    allocated_indices = malloc(sizeof(uint32_t) * total_blocks);
    TEST_ASSERT_NOT_NULL(allocated_indices);

    // Act: 循环分配，直到分配失败，并记录分配的块数
    uint32_t alloc_count = 0;
    while (nvm_slab_alloc(slab, &allocated_indices[alloc_count]) == 0) {
        alloc_count++;
        // 增加一个保护，防止无限循环
        if (alloc_count >= total_blocks + 1) {
            TEST_FAIL_MESSAGE("Allocation loop ran more times than total blocks.");
            break;
        }
    }

    // Assert: 验证分配的块数是否等于Slab的总容量
    TEST_ASSERT_EQUAL_UINT32(total_blocks, alloc_count);
    TEST_ASSERT_EQUAL_UINT32(total_blocks, slab->allocated_block_count);
    TEST_ASSERT_TRUE(nvm_slab_is_full(slab));
    
    // 在已满的 slab 上再次分配应该会失败
    ret = nvm_slab_alloc(slab, &block_idx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, ret, "Allocation on a full slab should fail.");


    // --- 子测试 5: 完全耗尽后再全部释放，并重新进行分配 ---
    // Act: 释放所有刚刚分配的块
    for (uint32_t i = 0; i < total_blocks; ++i) {
        nvm_slab_free(slab, allocated_indices[i]);
    }

    // Assert: slab 应该变回空的状态
    TEST_ASSERT_TRUE(nvm_slab_is_empty(slab));
    TEST_ASSERT_EQUAL_UINT32(0, slab->allocated_block_count);
    // 缓存中应该有 cache_size (64) 个块，因为大量的 free 操作会填满它
    TEST_ASSERT_EQUAL_UINT32(cache_size, slab->cache_count);

    // Act: 再次尝试将 slab 完全耗尽
    alloc_count = 0;
    while (nvm_slab_alloc(slab, &block_idx) == 0) {
        alloc_count++;
        if (alloc_count >= total_blocks + 1) {
            TEST_FAIL_MESSAGE("Re-allocation loop ran more times than total blocks.");
            break;
        }
    }

    // Assert: 应该能再次成功分配出所有块
    TEST_ASSERT_EQUAL_UINT32(total_blocks, alloc_count);
    TEST_ASSERT_TRUE(nvm_slab_is_full(slab));


    // Cleanup: 释放本次测试中使用的所有资源
    free(allocated_indices);
    nvm_slab_destroy(slab);
}


// 这是一个宏，用于简化调用辅助函数，并提供更好的失败信息
// Unity 没有直接支持参数化测试，我们用这种方式模拟
#define RUN_TEST_CASE(sc_id) \
    UnityPrint("Testing Size Class: "); \
    UnityPrintNumberUnsigned(sc_id); \
    UnityPrint("\n"); \
    perform_full_lifecycle_test_for_size(sc_id)



// 在 test_nvm_slab.c 文件中

// ... setUp, tearDown, 和其他测试用例 ...

/**
 * @brief 辅助函数，对给定 SizeClassID 的 Slab 进行完整的生命周期测试。
 * 
 * 这个函数会创建一个指定大小的 Slab，将其完全填满，再完全释放，
 * 验证每一步的状态是否正确。
 *
 * @param sc_id 要测试的大小类ID。
 */
static void perform_full_lifecycle_test_for_size(SizeClassID sc_id) {
    // 1. 创建 Slab
    NvmSlab* slab = nvm_slab_create(sc_id, 0);
    // 使用 Unity 的辅助宏，如果创建失败，则提供带参数的错误信息
    TEST_ASSERT_NOT_NULL_MESSAGE(slab, "Failed to create slab for size class ID.");

    const uint32_t total_blocks = slab->total_block_count;
    uint32_t block_idx;
    int ret;

    // 2. 验证总块数计算是否合理
    // (2MB / block_size) 应该等于 total_blocks
    // 这是一个很好的交叉验证
    uint32_t expected_block_size = get_block_size_from_sc_id(sc_id);
    TEST_ASSERT_EQUAL_UINT32( (SIMULATED_NVM_SIZE / expected_block_size), total_blocks );


    // 3. 将 Slab 完全填满
    for (uint32_t i = 0; i < total_blocks; ++i) {
        ret = nvm_slab_alloc(slab, &block_idx);
        if (ret != 0) {
            // 如果在填满前分配失败，立即报错并停止
            TEST_FAIL_MESSAGE("Allocation failed unexpectedly before slab was full.");
        }
    }
    
    // 4. 验证 Slab 是否已满
    TEST_ASSERT_TRUE_MESSAGE(nvm_slab_is_full(slab), "Slab should be full after allocating all blocks.");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(total_blocks, slab->allocated_block_count, "Allocated count should match total blocks when full.");
    // 尝试再次分配，应该会失败
    ret = nvm_slab_alloc(slab, &block_idx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, ret, "Allocation should fail on a full slab.");

    // 5. 将 Slab 完全释放
    // 注意：这里我们只知道块的数量，但不知道它们的索引。
    // 在一个真实的系统中，我们需要记录索引。在这个简化的测试中，
    // 我们假设我们的分配器总是从0开始顺序分配的（在没有缓存回收的情况下）。
    // 为了使测试更健壮，我们应该记录并释放真实的索引。
    uint32_t* indices = malloc(sizeof(uint32_t) * total_blocks);
    TEST_ASSERT_NOT_NULL(indices);
    nvm_slab_destroy(slab); // 先销毁旧的
    slab = nvm_slab_create(sc_id, 0); // 重新创建一个干净的
    
    for (uint32_t i = 0; i < total_blocks; ++i) {
        nvm_slab_alloc(slab, &indices[i]);
    }
    // 现在我们有了所有真实的索引，再释放它们
    for (uint32_t i = 0; i < total_blocks; ++i) {
        nvm_slab_free(slab, indices[i]);
    }
    free(indices);


    // 6. 验证 Slab 是否已空
    TEST_ASSERT_TRUE_MESSAGE(nvm_slab_is_empty(slab), "Slab should be empty after freeing all blocks.");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, slab->allocated_block_count, "Allocated count should be 0 when empty.");

    // 7. 清理
    nvm_slab_destroy(slab);
}


/**
 * @brief 测试不同块尺寸的 Slab 的核心功能。
 *
 * 这个测试用例会遍历多种不同的大小类，对每一种都执行一次
 * 创建 -> 填满 -> 释放 的完整生命周期测试。
 */
void test_slab_behavior_with_various_sizes(void) {
    // 测试一个较小的尺寸
    RUN_TEST_CASE(SC_8B);

    // 测试一个中等尺寸
    RUN_TEST_CASE(SC_128B);

    // 测试一个较大的尺寸
    RUN_TEST_CASE(SC_4K);
    
    // 你可以添加更多的大小类进行测试
    // RUN_TEST_CASE(SC_16B);
    // RUN_TEST_CASE(SC_32B);
    // RUN_TEST_CASE(SC_64B);
    // RUN_TEST_CASE(SC_256B);
    // RUN_TEST_CASE(SC_512B);
    // RUN_TEST_CASE(SC_1K);
    // RUN_TEST_CASE(SC_2K);
}




// ============================================================================
// main 函数 - 测试执行入口
// ============================================================================
int main(void) {
    UNITY_BEGIN();

    // 运行我们新编写的详尽测试用例
    RUN_TEST(test_nvm_slab_creation_and_destruction);
    RUN_TEST(test_slab_alloc_free_cache_behavior);
    RUN_TEST(test_slab_behavior_with_various_sizes);

    return UNITY_END();
}