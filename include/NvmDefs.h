#ifndef NVM_DEFS_H
#define NVM_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "NvmConfig.h"

// ============================================================================
//                          全局常量定义
// ============================================================================

// NVM 起始偏移量 (模拟环境下通常为 0)
#define NVM_START_OFFSET  0

// 标准 Slab 大小: 2MB (Huge Page Friendly)
#define NVM_SLAB_SIZE     (2 * 1024 * 1024)

// Slab 本地缓存 (FreeList) 配置
#define SLAB_CACHE_SIZE        64
#define SLAB_CACHE_BATCH_SIZE  (SLAB_CACHE_SIZE / 2)

// 哈希表初始容量 (建议为素数以减少冲突)
#define INITIAL_HASHTABLE_CAPACITY 101

// ============================================================================
//                          通用宏工具
// ============================================================================

// 向上对齐到 align (align 必须是 2 的幂)
#define NVM_ALIGN_UP(x, align) (((x) + ((align) - 1)) & ~((align) - 1))

// 向下对齐到 align
#define NVM_ALIGN_DOWN(x, align) ((x) & ~((align) - 1))

// 错误日志输出
#define LOG_ERR(fmt, ...) fprintf(stderr, "[NvmAllocator] Error: " fmt "\n", ##__VA_ARGS__)

// ============================================================================
//                          尺寸类别 (Size Classes)
// ============================================================================

typedef enum {
    SC_8B = 0,
    SC_16B,
    SC_32B,
    SC_64B,
    SC_128B,
    SC_256B,
    SC_512B,
    SC_1K,
    SC_2K,
    SC_4K,
    SC_COUNT    // 哨兵值：总类别数
} SizeClassID;

#ifdef __cplusplus
}
#endif

#endif // NVM_DEFS_H