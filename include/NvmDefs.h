#ifndef NVM_DEFS_H_
#define NVM_DEFS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h> 

#define NVM_START_OFFSET  0

#define NVM_SLAB_SIZE (2 * 1024 * 1024)  // 每个Slab的大小 (2MB)

#define SLAB_CACHE_SIZE (64)          // Slab本地缓存的大小
#define SLAB_CACHE_BATCH_SIZE (SLAB_CACHE_SIZE / 2) // 批量填充/回填缓存的大小

#define INITIAL_HASHTABLE_CAPACITY 101  // 初始哈希表容量，素数有助于减少哈希冲突

// 定义了内存分配的尺寸类别ID。
typedef enum {
    SC_8B,       // 8字节
    SC_16B,      // 16字节
    SC_32B,      // 32字节
    SC_64B,      // 64字节
    SC_128B,     // 128字节
    SC_256B,     // 256字节
    SC_512B,     // 512字节
    SC_1K,       // 1024字节
    SC_2K,       // 2048字节
    SC_4K,       // 4096字节
    SC_COUNT     // 尺寸类别的总数，也用作哨兵值
} SizeClassID;

#endif // NVM_DEFS_H_