#include "unity.h"

// 包含所有必要的头文件
#include "NvmDefs.h"
#include "NvmSlab.h"
#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmAllocator.h"

// 包含所有组件的实现文件，以进行白盒测试并创建单个测试可执行文件。
// 包含顺序很重要：先包含依赖项。
#include "NvmSlab.c"
#include "NvmSpaceManager.c"
#include "SlabHashTable.c"
#include "NvmAllocator.c"

#include <stdlib.h>

// 为集成测试定义一个较大的模拟NVM空间
#define TOTAL_NVM_SIZE (10 * NVM_SLAB_SIZE) // 20MB，足够容纳10个Slab
#define NUM_SLABS 10

// 由于每个测试都会创建和销毁自己的分配器实例，因此这些函数可以为空。
void setUp(void) {}
void tearDown(void) {}

// ============================================================================
//                          测试用例
// ============================================================================

/**
 * @brief 测试 NvmAllocator 的创建和销毁。
 */
void test_allocator_lifecycle(void) {
    // --- 子测试 1: 正常创建 ---
    NvmAllocator* allocator = nvm_allocator_create(TOTAL_NVM_SIZE, 0);
    TEST_ASSERT_NOT_NULL(allocator);

    // 白盒检查: 确认所有内部组件都已正确初始化
    TEST_ASSERT_NOT_NULL(allocator->space_manager);
    TEST_ASSERT_NOT_NULL(allocator->slab_lookup_table);
    TEST_ASSERT_EQUAL_UINT64(TOTAL_NVM_SIZE, allocator->space_manager->head->size); // 确认空间管理器有初始空间
    for (int i = 0; i < SC_COUNT; ++i) {
        TEST_ASSERT_NULL(allocator->slab_lists[i]); // 确认所有slab链表为空
    }

    nvm_allocator_destroy(allocator);

    // --- 子测试 2: 无效参数创建 ---
    allocator = nvm_allocator_create(NVM_SLAB_SIZE - 1, 0);
    TEST_ASSERT_NULL(allocator);

    // --- 子测试 3: 销毁 NULL 指针 ---
    nvm_allocator_destroy(NULL); // 不应崩溃
}

/**
 * @brief 测试基本的 malloc/free 和内部状态变化。
 */
void test_basic_malloc_and_free(void) {
    NvmAllocator* allocator = nvm_allocator_create(TOTAL_NVM_SIZE, 0);
    
    // --- 子测试 1: 首次分配 (应创建新Slab) ---
    uint64_t offset = nvm_malloc(allocator, 30); // 应使用 SC_32B
    TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offset);
    
    // 白盒检查:
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[SC_32B]); // 32B slab链表应非空
    TEST_ASSERT_EQUAL_UINT32(1, allocator->slab_lookup_table->count); // 哈希表应有1个条目
    // 空间管理器应分配了一个slab，剩余9个
    TEST_ASSERT_EQUAL_UINT64((NUM_SLABS - 1) * NVM_SLAB_SIZE, allocator->space_manager->head->size);

    // --- 子测试 2: 释放 ---
    nvm_free(allocator, offset);
    // 此时slab应未被回收，因为它是该尺寸的唯一一个
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[SC_32B]);
    TEST_ASSERT_TRUE(nvm_slab_is_empty(allocator->slab_lists[SC_32B]));

    nvm_allocator_destroy(allocator);
}

/**
 * @brief 测试Slab的创建和复用逻辑。
 */
void test_slab_creation_and_reuse(void) {
    NvmAllocator* allocator = nvm_allocator_create(TOTAL_NVM_SIZE, 0);

    // 1. 分配一个 60 字节的对象，应创建 SC_64B 的Slab
    uint64_t offset1 = nvm_malloc(allocator, 60);
    TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offset1);
    NvmSlab* slab64_ptr = allocator->slab_lists[SC_64B];
    TEST_ASSERT_NOT_NULL(slab64_ptr);
    TEST_ASSERT_EQUAL_UINT32(1, allocator->slab_lookup_table->count);

    // 2. 再次分配 60 字节，应复用同一个Slab
    uint64_t offset2 = nvm_malloc(allocator, 60);
    TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offset2);
    TEST_ASSERT_EQUAL_PTR(slab64_ptr, allocator->slab_lists[SC_64B]); // 确认头指针没变
    TEST_ASSERT_EQUAL_UINT32(1, allocator->slab_lookup_table->count); // 确认哈希表数量没变

    // 3. 分配一个不同尺寸的对象 (8字节)，应创建新的Slab
    uint64_t offset3 = nvm_malloc(allocator, 8);
    TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offset3);
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[SC_8B]);
    TEST_ASSERT_EQUAL_UINT32(2, allocator->slab_lookup_table->count); // 哈希表数量应变为2

    nvm_allocator_destroy(allocator);
}

/**
 * @brief 测试空Slab被回收的核心逻辑。
 */
void test_empty_slab_recycling(void) {
    NvmAllocator* allocator = nvm_allocator_create(TOTAL_NVM_SIZE, 0);

    // --- 1. 创建两个同尺寸的Slab ---
    size_t alloc_size = 128;
    SizeClassID sc_id = SC_128B;
    uint32_t blocks_per_slab = NVM_SLAB_SIZE / alloc_size;

    // 分配并填满第一个Slab
    uint64_t* offsets = malloc(sizeof(uint64_t) * (blocks_per_slab + 1));
    for (uint32_t i = 0; i < blocks_per_slab; ++i) {
        offsets[i] = nvm_malloc(allocator, alloc_size);
        TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offsets[i]);
    }
    
    // 白盒检查: 此时应只有一个SC_128B的Slab
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[sc_id]);
    TEST_ASSERT_NULL(allocator->slab_lists[sc_id]->next_in_chain);

    // 再分配一个，强制创建第二个Slab
    offsets[blocks_per_slab] = nvm_malloc(allocator, alloc_size);
    TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offsets[blocks_per_slab]);
    
    // 白盒检查: 此时应有两个SC_128B的Slab
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[sc_id]);
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[sc_id]->next_in_chain);
    TEST_ASSERT_EQUAL_UINT32(2, allocator->slab_lookup_table->count);
    
    // --- 2. 释放并回收第一个Slab ---
    // 释放第一个Slab的所有块
    for (uint32_t i = 0; i < blocks_per_slab; ++i) {
        nvm_free(allocator, offsets[i]);
    }

    // 白盒检查: 第一个Slab应已被回收
    // 因为链表中有2个Slab，所以空的那个会被回收
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[sc_id]); // 链表头应指向第二个Slab
    TEST_ASSERT_NULL(allocator->slab_lists[sc_id]->next_in_chain); // 链表现在只有一个节点
    TEST_ASSERT_EQUAL_UINT32(1, allocator->slab_lookup_table->count); // 哈希表也应只有一个条目
    
    // --- 3. 释放最后一个Slab，但不回收 ---
    nvm_free(allocator, offsets[blocks_per_slab]);
    
    // 白盒检查: 最后一个Slab变空了，但不应被回收
    TEST_ASSERT_NOT_NULL(allocator->slab_lists[sc_id]);
    TEST_ASSERT_NULL(allocator->slab_lists[sc_id]->next_in_chain);
    TEST_ASSERT_EQUAL_UINT32(1, allocator->slab_lookup_table->count);
    TEST_ASSERT_TRUE(nvm_slab_is_empty(allocator->slab_lists[sc_id])); // 确认它确实是空的

    // 清理
    free(offsets);
    nvm_allocator_destroy(allocator);
}

void test_parameter_and_error_handling(void) {
    NvmAllocator* allocator = nvm_allocator_create(TOTAL_NVM_SIZE, 0);

    // 1. 测试分配大小为 0
    uint64_t offset = nvm_malloc(allocator, 0);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, offset);

    // 2. 测试分配大小超过最大Slab块 (4K)
    offset = nvm_malloc(allocator, 4097); // 4096 is max block size
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, offset);

    // 3. 测试释放一个无效的地址 (不在任何Slab范围内)
    //    注意：这会触发 assert! 所以这个测试只能在Debug模式下运行，
    //    并且通常我们会把它放在一个单独的、预期会失败的测试集中。
    //    在常规测试中，我们只需确认调用不会导致崩溃。
    // nvm_free(allocator, TOTAL_NVM_SIZE + 100); // An address far outside our managed space

    // 4. 测试释放 NULL 或 -1
    // nvm_free(allocator, 0);
    nvm_free(allocator, (uint64_t)-1);
    
    nvm_allocator_destroy(allocator);
}

void test_nvm_space_exhaustion(void) {
    // 只创建一个能容纳2个Slab的空间
    NvmAllocator* allocator = nvm_allocator_create(2 * NVM_SLAB_SIZE, 0);

    // 创建并填满 slab1 (SC_8B)
    for (int i = 0; i < NVM_SLAB_SIZE / 8; ++i) {
        nvm_malloc(allocator, 8);
    }
    
    // 创建并填满 slab2 (SC_16B)
    for (int i = 0; i < NVM_SLAB_SIZE / 16; ++i) {
        nvm_malloc(allocator, 16);
    }

    // 白盒检查：此时底层空间应该已经用完
    TEST_ASSERT_NULL(allocator->space_manager->head);

    // 此时再请求一个不同尺寸的内存，需要创建新Slab，但底层空间已无
    uint64_t offset = nvm_malloc(allocator, 32);
    TEST_ASSERT_EQUAL_UINT64((uint64_t)-1, offset);

    nvm_allocator_destroy(allocator);
}

void test_mixed_load_and_fragmentation(void) {
    NvmAllocator* allocator = nvm_allocator_create(TOTAL_NVM_SIZE, 0);
    const int num_allocs = 1000;
    uint64_t* offsets = malloc(sizeof(uint64_t) * num_allocs);

    // 阶段1: 大量不同尺寸的分配
    for (int i = 0; i < num_allocs; ++i) {
        // (i % 100 + 1) 会产生 1-100 字节的随机大小请求
        offsets[i] = nvm_malloc(allocator, (i % 100) + 1);
        TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offsets[i]);
    }

    // 阶段2: 交错释放 (比如释放所有偶数索引的块)
    for (int i = 0; i < num_allocs; i += 2) {
        nvm_free(allocator, offsets[i]);
        offsets[i] = (uint64_t)-1; // 标记为已释放
    }

    // 阶段3: 再次分配，验证空间能否被复用
    for (int i = 0; i < num_allocs / 2; ++i) {
        uint64_t offset = nvm_malloc(allocator, (i % 50) + 1);
        TEST_ASSERT_NOT_EQUAL((uint64_t)-1, offset);
    }
    
    free(offsets);
    nvm_allocator_destroy(allocator);
}


// ============================================================================
//                          测试执行入口
// ============================================================================
int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_allocator_lifecycle);
    RUN_TEST(test_basic_malloc_and_free);
    RUN_TEST(test_slab_creation_and_reuse);
    RUN_TEST(test_empty_slab_recycling);
    RUN_TEST(test_parameter_and_error_handling);
    RUN_TEST(test_nvm_space_exhaustion);
    RUN_TEST(test_mixed_load_and_fragmentation);

    return UNITY_END();
}