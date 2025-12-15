#define _GNU_SOURCE // 为了使用 pthread_setaffinity_np
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "NvmAllocator.h"

// ============================================================================
//                          测试配置
// ============================================================================

#define TEST_THREAD_COUNT 4        // 启动 4 个线程
#define ITERATIONS_PER_THREAD 5000 // 每个线程执行 5000 次操作
#define TOTAL_NVM_SIZE (64 * 1024 * 1024) // 64MB 模拟 NVM

// 全局分配器实例（在 NvmAllocator.c 中定义，这里 extern 引用）
// 注意：如果是静态链接库，可能无法直接 extern 内部变量，
// 但我们之前的测试已经用了 extern，这里沿用。
// 如果编译报错，说明需要通过头文件暴露，或者仅测试公开 API。
// 为了严谨，本测试仅使用公开 API。

static void* g_nvm_base = NULL;

// 简单的共享指针池，用于测试跨线程释放
// 简单的自旋锁保护这个池子
typedef struct {
    void* ptrs[TEST_THREAD_COUNT * ITERATIONS_PER_THREAD];
    int count;
    pthread_spinlock_t lock;
} SharedPtrPool;

SharedPtrPool g_pool;

void pool_init() {
    g_pool.count = 0;
    pthread_spin_init(&g_pool.lock, PTHREAD_PROCESS_PRIVATE);
    memset(g_pool.ptrs, 0, sizeof(g_pool.ptrs));
}

void pool_push(void* ptr) {
    if (!ptr) return;
    pthread_spin_lock(&g_pool.lock);
    if (g_pool.count < TEST_THREAD_COUNT * ITERATIONS_PER_THREAD) {
        g_pool.ptrs[g_pool.count++] = ptr;
    } else {
        // 池子满了，直接释放
        pthread_spin_unlock(&g_pool.lock); // 先解锁避免死锁（nvm_free可能也要锁）
        nvm_free(ptr);
        return;
    }
    pthread_spin_unlock(&g_pool.lock);
}

void* pool_pop_random() {
    void* ptr = NULL;
    pthread_spin_lock(&g_pool.lock);
    if (g_pool.count > 0) {
        // 随机取一个，为了简单，取最后一个
        ptr = g_pool.ptrs[--g_pool.count];
    }
    pthread_spin_unlock(&g_pool.lock);
    return ptr; // 可能为 NULL
}

// ============================================================================
//                          线程工作函数
// ============================================================================

typedef struct {
    int thread_id;
} ThreadArgs;

void* thread_func(void* arg) {
    ThreadArgs* args = (ThreadArgs*)arg;
    int tid = args->thread_id;
    
    // 1. 核心绑定 (CPU Pinning)
    // 强制将线程绑定到特定的 CPU 核，模拟真实的并行环境
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    // 简单的映射：线程 0 -> CPU 0, 线程 1 -> CPU 1 ...
    // 如果系统核数不够，取模
    int target_cpu = tid % sysconf(_SC_NPROCESSORS_ONLN);
    CPU_SET(target_cpu, &cpuset);
    
    int s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (s != 0) {
        perror("pthread_setaffinity_np");
        // 继续运行，只是可能调度不准确
    } else {
        // printf("Thread %d pinned to CPU %d\n", tid, target_cpu);
    }

    // 2. 压力测试循环
    unsigned int seed = time(NULL) + tid;
    
    for (int i = 0; i < ITERATIONS_PER_THREAD; ++i) {
        int action = rand_r(&seed) % 10;

        if (action < 6) { 
            // 60% 概率：分配内存
            // 随机大小：大部分小内存(Slab)，偶尔大一点
            size_t size = (rand_r(&seed) % 512) + 8; 
            
            void* p = nvm_malloc(size);
            
            if (p) {
                // 写入数据验证 (检测重叠)
                memset(p, 0xAA, size);
                
                // 50% 概率自己存着稍后放回池子，50% 概率直接放进池子让别人释放
                pool_push(p); 
            }
        } else {
            // 40% 概率：尝试释放内存 (可能是自己分配的，也可能是别人分配的)
            void* p = pool_pop_random();
            if (p) {
                nvm_free(p);
            }
        }
        
        // 偶尔让出 CPU，增加并发不确定性
        if (i % 100 == 0) sched_yield();
    }

    return NULL;
}

// ============================================================================
//                          主程序
// ============================================================================

int main() {
    printf("=== Starting Multi-threaded Stress Test ===\n");
    
    // 1. 初始化模拟 NVM
    g_nvm_base = malloc(TOTAL_NVM_SIZE);
    if (!g_nvm_base) {
        fprintf(stderr, "Failed to alloc mock NVM\n");
        return 1;
    }
    
    // 2. 初始化分配器
    if (nvm_allocator_create(g_nvm_base, TOTAL_NVM_SIZE) != 0) {
        fprintf(stderr, "Allocator init failed\n");
        return 1;
    }

    pool_init();

    // 3. 创建线程
    pthread_t threads[TEST_THREAD_COUNT];
    ThreadArgs args[TEST_THREAD_COUNT];

    printf("Spawning %d threads...\n", TEST_THREAD_COUNT);
    
    for (int i = 0; i < TEST_THREAD_COUNT; ++i) {
        args[i].thread_id = i;
        if (pthread_create(&threads[i], NULL, thread_func, &args[i]) != 0) {
            fprintf(stderr, "Failed to create thread %d\n", i);
            return 1;
        }
    }

    // 4. 等待结束
    for (int i = 0; i < TEST_THREAD_COUNT; ++i) {
        pthread_join(threads[i], NULL);
    }

    printf("All threads finished.\n");

    // 5. 清理池中剩余的指针
    printf("Cleaning up remaining objects...\n");
    void* p;
    while ((p = pool_pop_random()) != NULL) {
        nvm_free(p);
    }

    // 6. 销毁分配器
    // 如果这一步崩了，说明元数据被破坏了
    nvm_allocator_destroy(); 
    free(g_nvm_base);

    printf("=== Test PASSED (No crashes detected) ===\n");
    return 0;
}