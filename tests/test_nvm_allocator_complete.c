/**
 * @file test_nvm_allocator_complete.c
 * @brief NVM 分配器全功能集成测试 (逻辑、边界、压力、并发)
 * 
 * 包含修复内容：
 * 1. test_allocator_lifecycle: 修复了 mock_nvm_base 未释放导致的 ASan 内存泄漏报错。
 * 2. test_multithread_remote_free: 修复了生产者-消费者模式下直接读写共享指针导致的 TSan 数据竞争报错，改为使用 __atomic 原子操作。
 */

#include "unity.h"

// ============================================================================
//                          头文件与源码包含
// ============================================================================
// 注意：这里采用包含 .c 文件的方式进行“白盒测试”，以便访问内部静态变量和结构体成员。
// 在编译时，请仅编译本文件，不要重复链接 NvmSlab.o 等目标文件。

#include "NvmDefs.h"
#include "NvmSlab.h"
#include "NvmSpaceManager.h"
#include "SlabHashTable.h"
#include "NvmAllocator.h"

// 直接包含实现文件
#include "NvmSlab.c"
#include "NvmSpaceManager.c"
#include "SlabHashTable.c"
#include "NvmAllocator.c"

#define _GNU_SOURCE // 为了使用 sched_setaffinity
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

// ============================================================================
//                          全局配置与 Setup/Teardown
// ============================================================================

#define MAX_BLOCK_SIZE 4096
// 默认 NVM 大小：20MB，足够容纳多个 Slab 和元数据
#define TOTAL_NVM_SIZE (10 * 1024 * 1024) 
#define NUM_SLABS_ESTIMATE (TOTAL_NVM_SIZE / NVM_SLAB_SIZE)

static void* mock_nvm_base = NULL;
extern struct NvmAllocator* global_nvm_allocator;

void setUp(void) {
    // 每次测试前分配一块清零的 DRAM 模拟 NVM
    if (mock_nvm_base == NULL) {
        mock_nvm_base = malloc(TOTAL_NVM_SIZE);
    }
    TEST_ASSERT_NOT_NULL(mock_nvm_base);
    memset(mock_nvm_base, 0, TOTAL_NVM_SIZE);
    
    int result = nvm_allocator_create(mock_nvm_base, TOTAL_NVM_SIZE);
    TEST_ASSERT_EQUAL_INT(0, result);
}

void tearDown(void) {
    nvm_allocator_destroy();
    if (mock_nvm_base) {
        free(mock_nvm_base);
        mock_nvm_base = NULL;
    }
}

// 辅助函数：重新初始化以获得更大的空间（用于压力测试）
void reinit_allocator_with_size(size_t new_size) {
    tearDown(); // 清理默认环境
    
    mock_nvm_base = malloc(new_size);
    TEST_ASSERT_NOT_NULL(mock_nvm_base);
    memset(mock_nvm_base, 0, new_size);
    
    int result = nvm_allocator_create(mock_nvm_base, new_size);
    TEST_ASSERT_EQUAL_INT(0, result);
}

// ============================================================================
//                          1. 基础逻辑与生命周期测试
// ============================================================================

void test_allocator_lifecycle(void) {
    TEST_ASSERT_NOT_NULL(global_nvm_allocator);
    TEST_ASSERT_EQUAL_PTR(mock_nvm_base, global_nvm_allocator->central_heap.nvm_base_addr);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->central_heap.space_manager);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->central_heap.slab_lookup_table);
    
    for (int i = 0; i < SC_COUNT; ++i) {
        TEST_ASSERT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[i]);
    }

    // --- 修复开始 ---
    nvm_allocator_destroy(); 
    
    // 关键修正：在置空指针前，必须先释放内存！(修复 ASan Leak 报错)
    if (mock_nvm_base) {
        free(mock_nvm_base);
    }
    mock_nvm_base = NULL; 
    
    // 现在指针是 NULL，测试传入 NULL 的情况
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_create(NULL, TOTAL_NVM_SIZE)); 
    
    // 重新分配内存以测试参数错误的情况
    mock_nvm_base = malloc(TOTAL_NVM_SIZE); 
    TEST_ASSERT_NOT_NULL(mock_nvm_base); // 确保 malloc 成功
    
    // 测试大小不足的情况
    TEST_ASSERT_EQUAL_INT(-1, nvm_allocator_create(mock_nvm_base, NVM_SLAB_SIZE - 1)); 
    
    // 恢复正常环境，以便 tearDown 能正常工作或进行后续检查
    // 注意：这里 create 会成功，tearDown 会负责最后的 destroy 和 free
    int res = nvm_allocator_create(mock_nvm_base, TOTAL_NVM_SIZE);
    TEST_ASSERT_EQUAL_INT(0, res);
    // --- 修复结束 ---
}

void test_basic_malloc_and_free(void) {
    void* ptr = nvm_malloc(30); // 应该分配 32B Class
    TEST_ASSERT_NOT_NULL(ptr);
    
    // 验证是否已创建对应的 Slab
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_32B]); 
    // 验证 Hash 表中是否记录了该 Slab
    TEST_ASSERT_EQUAL_UINT32(1, global_nvm_allocator->central_heap.slab_lookup_table->count);

    nvm_free(ptr);
    
    // 释放后 Slab 还在，但应该是空的（或者根据策略只有1个空闲块）
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_32B]);
    TEST_ASSERT_TRUE(nvm_slab_is_empty(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_32B]));
}

void test_slab_creation_and_reuse(void) {
    void* ptr1 = nvm_malloc(60); // 64B
    TEST_ASSERT_NOT_NULL(ptr1);
    NvmSlab* slab64_ptr = global_nvm_allocator->cpu_heaps[0].slab_lists[SC_64B];
    TEST_ASSERT_NOT_NULL(slab64_ptr);

    void* ptr2 = nvm_malloc(60); // 应该复用同一个 64B Slab
    TEST_ASSERT_NOT_NULL(ptr2);
    TEST_ASSERT_EQUAL_PTR(slab64_ptr, global_nvm_allocator->cpu_heaps[0].slab_lists[SC_64B]);

    void* ptr3 = nvm_malloc(8); // 8B, 新的 Size Class
    TEST_ASSERT_NOT_NULL(ptr3);
    TEST_ASSERT_NOT_NULL(global_nvm_allocator->cpu_heaps[0].slab_lists[SC_8B]);
    
    // 现在应该有 2 个 Slab 在 Hash 表中
    TEST_ASSERT_EQUAL_UINT32(2, global_nvm_allocator->central_heap.slab_lookup_table->count);
    
    nvm_free(ptr1);
    nvm_free(ptr2);
    nvm_free(ptr3);
}

void test_empty_slab_recycling(void) {
    size_t alloc_size = 128;
    SizeClassID sc_id = SC_128B;
    // 计算填满一个 Slab 需要多少块
    uint32_t blocks_per_slab = NVM_SLAB_SIZE / alloc_size; // 近似值，实际要减去元数据头
    
    int total_objs = 20000; // 足够多，确保跨越多个 Slab
    void** ptrs = malloc(sizeof(void*) * total_objs);
    int allocated_count = 0;
    
    // 1. 填满第一个 Slab 并触发第二个 Slab
    for (int i = 0; i < total_objs; ++i) {
        ptrs[i] = nvm_malloc(alloc_size);
        if (ptrs[i] == NULL) break;
        allocated_count++;
    }
    
    TEST_ASSERT_TRUE_MESSAGE(allocated_count > 100, "Allocation failed too early");

    NvmSlab* head_slab = global_nvm_allocator->cpu_heaps[0].slab_lists[sc_id];
    TEST_ASSERT_NOT_NULL(head_slab);
    
    // 2. 全部释放
    for (int i = 0; i < allocated_count; ++i) {
        nvm_free(ptrs[i]);
    }
    
    // 3. 验证头部 Slab 是否为空
    head_slab = global_nvm_allocator->cpu_heaps[0].slab_lists[sc_id];
    if (head_slab) {
        TEST_ASSERT_TRUE(nvm_slab_is_empty(head_slab));
    }

    free(ptrs);
}

// ============================================================================
//                          2. 边界、错误处理与对齐测试
// ============================================================================

void test_parameter_and_error_handling(void) {
    TEST_ASSERT_NULL(nvm_malloc(0));
    // 假设最大块限制
    TEST_ASSERT_NULL(nvm_malloc(MAX_BLOCK_SIZE + 1));
    // 释放 NULL 不应崩溃
    nvm_free(NULL); 
}

void test_nvm_space_exhaustion(void) {
    // 使用极小的 NVM 空间重置分配器
    reinit_allocator_with_size(3 * NVM_SLAB_SIZE); // 约 6MB

    // 尝试耗尽内存
    int count = 0;
    while (1) {
        if (nvm_malloc(4096) == NULL) break;
        count++;
    }
    TEST_ASSERT_TRUE_MESSAGE(count > 0, "Should be able to allocate some blocks");
    
    // 此时应该无法再分配
    TEST_ASSERT_NULL(nvm_malloc(4096));
}

void test_alignment_integrity(void) {
    // 验证 1B 到 64B 的分配是否都按 8 字节对齐
    for (int i = 1; i <= 64; i++) {
        void* ptr = nvm_malloc(i);
        TEST_ASSERT_NOT_NULL(ptr);
        
        uintptr_t addr = (uintptr_t)ptr;
        TEST_ASSERT_EQUAL_MESSAGE(0, addr % 8, "Memory address must be 8-byte aligned");
        
        nvm_free(ptr);
    }
}

void test_size_class_boundaries(void) {
    // 边界值测试
    void* p8 = nvm_malloc(8);
    TEST_ASSERT_NOT_NULL(p8);
    
    void* p9 = nvm_malloc(9); // 应该升级到 16B
    TEST_ASSERT_NOT_NULL(p9);
    
    void* p_max = nvm_malloc(MAX_BLOCK_SIZE);
    TEST_ASSERT_NOT_NULL(p_max);
    
    nvm_free(p8);
    nvm_free(p9);
    nvm_free(p_max);
}

// ============================================================================
//                          3. 单线程压力与碎片化测试
// ============================================================================

void test_mixed_load_and_fragmentation(void) {
    // 加大测试量
    const int num_allocs = 5000;
    void** ptrs = calloc(num_allocs, sizeof(void*));
    TEST_ASSERT_NOT_NULL(ptrs);

    // 1. 随机大小分配
    for (int i = 0; i < num_allocs; ++i) {
        ptrs[i] = nvm_malloc((rand() % 256) + 1);
        // 允许部分失败（如果空间不足），但不应 Crash
    }

    // 2. 间隔释放 (制造碎片孔洞)
    for (int i = 0; i < num_allocs; i += 2) {
        if (ptrs[i]) {
            nvm_free(ptrs[i]);
            ptrs[i] = NULL;
        }
    }

    // 3. 再次填补
    for (int i = 0; i < num_allocs; i += 2) {
        ptrs[i] = nvm_malloc((rand() % 128) + 1);
    }
    
    // 4. 全部释放
    for (int i = 0; i < num_allocs; ++i) {
        if (ptrs[i]) nvm_free(ptrs[i]);
    }
    
    free(ptrs);
}

void test_stress_random_ops(void) {
    // 纯随机“捣乱”测试
    const int NUM_SLOTS = 1000;
    const int ITERATIONS = 50000;
    void* slots[1000] = {0};

    srand(0xDEADBEEF); // 固定种子以便复现

    for (int i = 0; i < ITERATIONS; ++i) {
        int idx = rand() % NUM_SLOTS;
        
        if (slots[idx] == NULL) {
            // Alloc
            size_t sz = (rand() % 1024) + 1;
            slots[idx] = nvm_malloc(sz);
        } else {
            // Free
            nvm_free(slots[idx]);
            slots[idx] = NULL;
        }
    }

    // 清理
    for (int i = 0; i < NUM_SLOTS; ++i) {
        if (slots[i]) nvm_free(slots[i]);
    }
}

// ============================================================================
//                          4. 多线程并发测试
// ============================================================================

typedef struct {
    int thread_id;
    int num_ops;
    volatile int error_count;
    void** shared_ptrs;
} ThreadArg;

// 场景 A: 独立并发 (各玩各的)
void* thread_func_independent(void* arg) {
    ThreadArg* t_arg = (ThreadArg*)arg;
    unsigned int seed = t_arg->thread_id; // 线程局部随机种子

    for (int i = 0; i < t_arg->num_ops; ++i) {
        size_t size = (rand_r(&seed) % 512) + 8;
        void* p = nvm_malloc(size);
        
        if (p) {
            // 简单的写入检查
            *(volatile char*)p = (char)t_arg->thread_id;
            // 模拟持有时间
            for(volatile int k=0; k<50; k++);
            // 释放
            nvm_free(p);
        }
    }
    return NULL;
}

void test_multithread_independent(void) {
    // 准备更大的空间供多线程霍霍
    reinit_allocator_with_size(50 * 1024 * 1024); // 50MB

    const int NUM_THREADS = 16;
    const int OPS = 50000;
    pthread_t threads[NUM_THREADS];
    ThreadArg args[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; ++i) {
        args[i].thread_id = i;
        args[i].num_ops = OPS;
        args[i].error_count = 0;
        pthread_create(&threads[i], NULL, thread_func_independent, &args[i]);
    }

    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(threads[i], NULL);
        TEST_ASSERT_EQUAL_INT(0, args[i].error_count);
    }
}

// 场景 B: 生产者-消费者 (测试 Remote Free / 锁竞争)
// 线程 0 (CPU 0) 申请 -> 线程 1 (CPU 1) 释放
// 修复：使用原子操作解决 TSan 报错
void* thread_producer(void* arg) {
    ThreadArg* t_arg = (ThreadArg*)arg;
    
    // 绑定到 CPU 0
    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(0, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

    for (int i = 0; i < t_arg->num_ops; ++i) {
        // 等待消费者取走数据 (shared_ptrs[i] 变为 NULL)
        // 使用原子加载，避免 TSan 报 Data Race
        while (__atomic_load_n(&t_arg->shared_ptrs[i], __ATOMIC_ACQUIRE) != NULL) {
            sched_yield();
        }
        
        void* p = nvm_malloc(64);
        if (!p) { t_arg->error_count++; break; }
        
        memset(p, 0xAA, 64);
        
        // 发布指针
        // 使用原子存储，确保 p 的初始化对消费者可见
        __atomic_store_n(&t_arg->shared_ptrs[i], p, __ATOMIC_RELEASE);
    }
    return NULL;
}

void* thread_consumer(void* arg) {
    ThreadArg* t_arg = (ThreadArg*)arg;
    
    // 绑定到 CPU 1 (确保这是 Remote Free)
    cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(1, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);

    for (int i = 0; i < t_arg->num_ops; ++i) {
        // 等待生产者生产
        void* p = NULL;
        // 使用原子加载
        while ((p = __atomic_load_n(&t_arg->shared_ptrs[i], __ATOMIC_ACQUIRE)) == NULL) {
            sched_yield();
        }
                
        // 检查数据
        if (*(unsigned char*)p != 0xAA) t_arg->error_count++;

        // 跨线程释放!
        nvm_free(p);
        
        // 标记槽位为空，通知生产者
        // 使用原子存储
        __atomic_store_n(&t_arg->shared_ptrs[i], NULL, __ATOMIC_RELEASE);
    }
    return NULL;
}

void test_multithread_remote_free(void) {
    reinit_allocator_with_size(50 * 1024 * 1024);

    const int OPS = 50000;
    pthread_t t_prod, t_cons;
    ThreadArg arg_prod, arg_cons;
    
    // 共享缓冲区
    void** buffer = calloc(OPS, sizeof(void*));
    
    arg_prod.num_ops = OPS;
    arg_prod.shared_ptrs = buffer;
    arg_prod.error_count = 0;

    arg_cons.num_ops = OPS;
    arg_cons.shared_ptrs = buffer;
    arg_cons.error_count = 0;

    pthread_create(&t_prod, NULL, thread_producer, &arg_prod);
    pthread_create(&t_cons, NULL, thread_consumer, &arg_cons);

    pthread_join(t_prod, NULL);
    pthread_join(t_cons, NULL);

    TEST_ASSERT_EQUAL_INT(0, arg_prod.error_count);
    TEST_ASSERT_EQUAL_INT(0, arg_cons.error_count);

    free(buffer);
}

// ============================================================================
//                          主函数：执行所有测试
// ============================================================================

int main(void) {
    UNITY_BEGIN();

    // ---------------------------------------------------------
    // 第一阶段：单线程确定性测试 (绑定 CPU 0)
    // ---------------------------------------------------------
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("WARNING: sched_setaffinity failed, tests may not be deterministic");
    }

    printf("=== Running Logic & Boundary Tests (Single Thread) ===\n");
    RUN_TEST(test_allocator_lifecycle);
    RUN_TEST(test_basic_malloc_and_free);
    RUN_TEST(test_slab_creation_and_reuse);
    RUN_TEST(test_empty_slab_recycling);
    RUN_TEST(test_parameter_and_error_handling);
    RUN_TEST(test_size_class_boundaries);
    RUN_TEST(test_alignment_integrity);
    RUN_TEST(test_nvm_space_exhaustion);
    
    printf("=== Running Stress Tests (Single Thread) ===\n");
    RUN_TEST(test_mixed_load_and_fragmentation);
    RUN_TEST(test_stress_random_ops);

    // ---------------------------------------------------------
    // 第二阶段：多线程并发测试 (解除 CPU 绑定或在测试内部绑定)
    // ---------------------------------------------------------
    cpu_set_t all_cpus;
    CPU_ZERO(&all_cpus);
    // 简单的开启前8个核，或者保持默认调度
    for(int i=0; i<8; i++) CPU_SET(i, &all_cpus);
    sched_setaffinity(0, sizeof(all_cpus), &all_cpus);

    printf("=== Running Concurrency Tests (Multi Thread) ===\n");
    RUN_TEST(test_multithread_independent);
    RUN_TEST(test_multithread_remote_free);

    return UNITY_END();
}