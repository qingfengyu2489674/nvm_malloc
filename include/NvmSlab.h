#ifndef NVM_SLAB_H
#define NVM_SLAB_H

#ifdef __cplusplus
extern "C" {
#endif

#include "NvmDefs.h"
#include <stdbool.h> 

// ============================================================================
//                          核心数据结构
// ============================================================================

/**
 * @brief NVM Slab 元数据结构
 * 
 * 管理 NVM 中的一个固定大小的内存页 (2MB)，将其切分为固定大小的小块。
 * 包含 DRAM 中的元数据、自旋锁、本地缓存 (FreeList) 和位图。
 */
typedef struct NvmSlab {
    
    // --- 1. 链表链接 ---
    // 指向同尺寸类别 (Size Class) 链表中的下一个 Slab
    // 仅被拥有该 Slab 的 CPU 在本地堆中访问，或在创建/销毁时访问
    struct NvmSlab* next_in_chain;

    // --- 2. 并发控制 ---
    // 保护位图 (bitmap) 和 本地缓存 (free_block_buffer) 的并发访问
    // 处理 Remote Free (跨线程释放) 时的竞争
    nvm_spinlock_t lock;

    // --- 3. 核心元数据 ---
    uint64_t nvm_base_offset;         // Slab 在 NVM 物理空间中的起始偏移量
    uint8_t  size_type_id;            // 对应的 SizeClassID
    uint8_t  _padding[3];             // 内存对齐填充 (保证后续 uint32 对齐)
    uint32_t block_size;              // 每个块的大小 (字节)
    uint32_t total_block_count;       // 该 Slab 能容纳的总块数
    uint32_t allocated_block_count;   // 当前已分配的块数 (用于判断是否满/空)

    // --- 4. 本地缓存 (Software Cache / FreeList) ---
    // 使用环形缓冲区作为一个固定大小的 LIFO/FIFO 缓存
    // 用于加速分配和释放，减少位图扫描的开销
    uint32_t cache_head;
    uint32_t cache_tail;
    uint32_t cache_count;
    uint32_t free_block_buffer[SLAB_CACHE_SIZE];

    // --- 5. 位图区域 (Flexible Array Member) ---
    // 必须位于结构体末尾。用于记录所有块的分配状态 (0=空闲, 1=占用)
    // 实际大小在创建时根据 block_size 动态计算分配
    unsigned char bitmap[];

} NvmSlab;


#define IS_BIT_SET(bitmap, n)   ((bitmap[(n) / 8] >> ((n) % 8)) & 1)
#define SET_BIT(bitmap, n)      (bitmap[(n) / 8] |= (1 << ((n) % 8)))
#define CLEAR_BIT(bitmap, n)    (bitmap[(n) / 8] &= ~(1 << ((n) % 8)))

// ============================================================================
//                          生命周期管理
// ============================================================================

/**
 * @brief 创建并初始化 Slab 元数据 (DRAM)
 * @param sc_id 尺寸类别 ID
 * @param nvm_base_offset NVM 上的物理起始偏移
 * @return 成功返回指针，失败返回 NULL
 */
NvmSlab* nvm_slab_create(SizeClassID sc_id, uint64_t nvm_base_offset);

/**
 * @brief 销毁 Slab 元数据
 * 注意：不负责释放 NVM 物理空间，仅释放 DRAM 元数据
 */
void nvm_slab_destroy(NvmSlab* self);

// ============================================================================
//                          核心操作 API
// ============================================================================

/**
 * @brief 从 Slab 中分配一个块
 * @param out_block_idx [输出] 分配到的块索引
 * @return 0 成功, -1 失败 (Slab 已满)
 */
int nvm_slab_alloc(NvmSlab* self, uint32_t* out_block_idx);

/**
 * @brief 归还一个块到 Slab
 * @param block_idx 块索引
 */
void nvm_slab_free(NvmSlab* self, uint32_t block_idx);

// ============================================================================
//                          状态查询与恢复 API
// ============================================================================

/**
 * @brief 手动设置位图状态 (用于故障恢复)
 * 将指定索引的块标记为已占用
 */
int nvm_slab_set_bitmap_at_idx(NvmSlab* self, uint32_t block_idx);

/**
 * @brief 检查 Slab 是否已满
 * @note 这是一个乐观检查 (Relaxed Read)，通常不加锁
 */
bool nvm_slab_is_full(const NvmSlab* self);

/**
 * @brief 检查 Slab 是否完全为空
 * @note 这是一个乐观检查
 */
bool nvm_slab_is_empty(const NvmSlab* self);

#ifdef __cplusplus
}
#endif

#endif // NVM_SLAB_H