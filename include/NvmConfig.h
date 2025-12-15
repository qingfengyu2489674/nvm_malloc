#ifndef NVM_CONFIG_H
#define NVM_CONFIG_H

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <bits/pthreadtypes.h>

// ============================================================================
// 1. CPU 配置与 ID 获取
// ============================================================================

// 定义最大支持的 CPU 核心数
// Linux 下可以设大一些，RTEMS 下根据实际 BSP 配置
#define MAX_CPUS 64

// 获取当前 CPU ID 的抽象函数
// 返回值范围保证: [0, MAX_CPUS - 1]
static inline int NVM_GET_CURRENT_CPU_ID() {
#ifdef __linux__
    int cpu = sched_getcpu();
    if (cpu < 0) return 0; // 异常保护
    if (cpu >= MAX_CPUS) return cpu % MAX_CPUS; // 简单的越界映射，防止溢出
    return cpu;
#elif defined(__rtems__)
    // RTEMS 实现占位
    // return rtems_scheduler_get_processor();
    return 0; 
#else
    // 默认单线程环境
    return 0;
#endif
}

// ============================================================================
// 2. 锁的原语封装 (OSAL)
// ============================================================================

// --- 类型 A: 自旋锁 (Spinlock) ---
// 适用场景: 持有时间极短、不能睡眠的场景 (如 Slab 内部操作)
typedef pthread_spinlock_t nvm_spinlock_t;

#define NVM_SPINLOCK_INIT(l)    pthread_spin_init(l, PTHREAD_PROCESS_PRIVATE)
#define NVM_SPINLOCK_DESTROY(l) pthread_spin_destroy(l)
#define NVM_SPINLOCK_ACQUIRE(l) pthread_spin_lock(l)
#define NVM_SPINLOCK_RELEASE(l) pthread_spin_unlock(l)

// --- 类型 B: 互斥锁 (Mutex) ---
// 适用场景: 持有时间较长、可能涉及系统调用的场景 (如 SpaceManager 扩容)
typedef pthread_mutex_t nvm_mutex_t;

#define NVM_MUTEX_INIT(l)       pthread_mutex_init(l, NULL)
#define NVM_MUTEX_DESTROY(l)    pthread_mutex_destroy(l)
#define NVM_MUTEX_ACQUIRE(l)    pthread_mutex_lock(l)
#define NVM_MUTEX_RELEASE(l)    pthread_mutex_unlock(l)

#define CACHE_LINE_SIZE 64 // 或 128，取决于你的 CPU 架构

#endif // NVM_CONFIG_H