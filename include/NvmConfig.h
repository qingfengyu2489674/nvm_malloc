#ifndef NVM_CONFIG_H
#define NVM_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
//                          系统头文件依赖
// ============================================================================

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <stdint.h>

// ============================================================================
//                          硬件与性能配置
// ============================================================================

// 最大支持的 CPU 核心数
// Linux: 通常设为系统逻辑核心数
// RTEMS: 根据 BSP 配置设定
#define MAX_CPUS 64

// 缓存行大小 (用于填充对齐，消除 False Sharing)
// x86_64 通常为 64，部分 ARM/PowerPC 为 128
#define CACHE_LINE_SIZE 64

// 分支预测优化宏
#if defined(__GNUC__) || defined(__clang__)
    #define NVM_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define NVM_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
    #define NVM_LIKELY(x)   (x)
    #define NVM_UNLIKELY(x) (x)
#endif

// ============================================================================
//                          OS 适配层 (CPU ID)
// ============================================================================

/**
 * @brief 获取当前线程运行的 CPU ID
 * @return 范围 [0, MAX_CPUS - 1]
 */
static inline int nvm_get_current_cpu_id(void) {
#ifdef __linux__
    int cpu = sched_getcpu();
    if (NVM_UNLIKELY(cpu < 0)) return 0;
    // 简单的取模映射，防止系统核数超过 MAX_CPUS 导致越界
    if (NVM_UNLIKELY(cpu >= MAX_CPUS)) return cpu % MAX_CPUS;
    return cpu;
#elif defined(__rtems__)
    // RTEMS 适配接口 (需根据实际 RTEMS 版本启用)
    // return rtems_scheduler_get_processor();
    return 0; 
#else
    // 默认/单线程环境
    return 0;
#endif
}

// 兼容旧代码的宏定义 (如果不想修改所有调用处)
#define NVM_GET_CURRENT_CPU_ID() nvm_get_current_cpu_id()

// ============================================================================
//                          OS 适配层 (锁原语)
// ============================================================================

// --- 1. 自旋锁 (Spinlock) ---
// 场景: 持有时间极短、不可睡眠 (如 Slab 位图操作)
typedef pthread_spinlock_t nvm_spinlock_t;

#define NVM_SPINLOCK_INIT(l)     pthread_spin_init(l, PTHREAD_PROCESS_PRIVATE)
#define NVM_SPINLOCK_DESTROY(l)  pthread_spin_destroy(l)
#define NVM_SPINLOCK_ACQUIRE(l)  pthread_spin_lock(l)
#define NVM_SPINLOCK_RELEASE(l)  pthread_spin_unlock(l)

// --- 2. 互斥锁 (Mutex) ---
// 场景: 持有时间较长、涉及系统调用 (如 SpaceManager 扩容)
typedef pthread_mutex_t nvm_mutex_t;

#define NVM_MUTEX_INIT(l)        pthread_mutex_init(l, NULL)
#define NVM_MUTEX_DESTROY(l)     pthread_mutex_destroy(l)
#define NVM_MUTEX_ACQUIRE(l)     pthread_mutex_lock(l)
#define NVM_MUTEX_RELEASE(l)     pthread_mutex_unlock(l)

// --- 3. 读写锁 (RWLock) ---
// 场景: 读多写少 (如全局 Slab 哈希表查找)
typedef pthread_rwlock_t nvm_rwlock_t;

#define NVM_RWLOCK_INIT(l)       pthread_rwlock_init(l, NULL)
#define NVM_RWLOCK_DESTROY(l)    pthread_rwlock_destroy(l)
#define NVM_RWLOCK_READ_LOCK(l)  pthread_rwlock_rdlock(l)
#define NVM_RWLOCK_WRITE_LOCK(l) pthread_rwlock_wrlock(l)
#define NVM_RWLOCK_UNLOCK(l)     pthread_rwlock_unlock(l)

#ifdef __cplusplus
}
#endif

#endif // NVM_CONFIG_H