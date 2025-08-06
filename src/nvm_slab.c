#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "nvm_slab.h"

#define IS_BIT_SET(bitmap, n) ((bitmap[(n) / 8] >> ((n) % 8)) & 1)
#define SET_BIT(bitmap, n) (bitmap[(n) / 8] |= (1 << ((n) % 8)))
#define CLEAR_BIT(bitmap, n) (bitmap[(n) / 8] &= ~(1 << ((n) % 8)))

static uint32_t get_block_size_from_sc_id(SizeClassID sc_id) {
    // 使用一个静态查找表可以比switch更快，但switch更具可读性
    switch (sc_id) {
        case SC_8B:   return 8;
        case SC_16B:  return 16;
        case SC_32B:  return 32;
        case SC_64B:  return 64;
        case SC_128B: return 128;
        case SC_256B: return 256;
        case SC_512B: return 512;
        case SC_1K:   return 1024;
        case SC_2K:   return 2048;
        case SC_4K:   return 4096;
        default:      return 0; // 表示这是一个无效的ID
    }
}



NvmSlab* nvm_slab_create(SizeClassID sc_id, uint64_t nvm_base_offset) {
    // --- 步骤 1: 根据 sc_id 计算Slab的几何参数 ---
    
    uint32_t block_size = get_block_size_from_sc_id(sc_id);
    if (block_size == 0) {
        fprintf(stderr, "Error: Invalid SizeClassID provided to nvm_slab_create.\n");
        return NULL;
    }

    uint32_t total_block_count = SLAB_TOTAL_SIZE / block_size;

    size_t bitmap_bytes = (total_block_count + 7) / 8;

    // --- 步骤 2: 在DRAM中为元数据分配内存 ---

    size_t total_alloc_size = sizeof(NvmSlab) + bitmap_bytes;

    NvmSlab* self = (NvmSlab*)calloc(1, total_alloc_size);

    // --- 步骤 3: 检查内存分配是否成功 ---
    if (self == NULL) {
        fprintf(stderr, "Error: Failed to allocate memory for NvmSlab metadata.\n");
        return NULL;
    }

    // --- 步骤 4: 初始化结构体的所有成员变量 ---

    // 设置从参数传入的值
    self->nvm_base_offset = nvm_base_offset;
    self->size_type_id = (uint8_t)sc_id;

    // 设置我们计算出来的值
    self->block_size = block_size;
    self->total_block_count = total_block_count;

    // --- 步骤 5: 返回创建好的 NvmSlab 指针 ---
    return self;
}

void nvm_slab_destroy(NvmSlab* self) {
    // 步骤 1 & 2: 检查指针并释放内存
    
    // C语言的 free() 函数被设计为可以安全地处理 NULL 指针。
    // 如果 self 为 NULL，free(self) 不会做任何事情。
    // 因此，一个明确的 `if (self != NULL)` 检查虽然清晰，但并非必需。
    // 为了代码的简洁性和利用标准库的特性，我们可以直接调用 free。
    free(self);

    // 调用者有责任在调用此函数后，将他们自己手中的指针设置为 NULL，
    // 以防止“悬垂指针”（dangling pointer）问题。
    // 例如：
    // nvm_slab_destroy(my_slab);
    // my_slab = NULL;
}

/**
 * @brief (内部函数) 从位图扫描空闲块，填充到DRAM缓存中。
 *
 * 当DRAM缓存为空时，此函数被调用。它会扫描位图，寻找最多
 * SLAB_CACHE_BATCH_SIZE 个空闲块，并将它们的索引放入缓存中。
 *
 * @param self 指向要操作的 NvmSlab 对象的指针。
 * @return 成功填充到缓存中的块的数量。如果Slab已满，则返回0。
 */
static uint32_t refill_cache(NvmSlab* self) {
    // 如果Slab理论上已经满了，就没必要扫描了
    if (self->allocated_block_count >= self->total_block_count) {
        return 0;
    }

    uint32_t filled_count = 0;
    // 扫描位图，寻找空闲块 (位为0的块)
    for (uint32_t i = 0; i < self->total_block_count && filled_count < SLAB_CACHE_BATCH_SIZE; ++i) {
        if (!IS_BIT_SET(self->bitmap, i)) {
            
            // 1. 将其索引放入缓存的尾部
            self->free_block_buffer[self->cache_tail] = i;
            
            // 2. 更新缓存的尾部索引 (环形)
            self->cache_tail = (self->cache_tail + 1) % SLAB_CACHE_SIZE;
            
            // 3. 将位图中的对应位设置为1，表示该块已被“预留”到缓存中，
            //    防止下次refill时重复扫描到它。
            SET_BIT(self->bitmap, i);

            filled_count++;
        }
    }

    // 4. 更新缓存中的总数
    self->cache_count += filled_count;

    return filled_count;
}


int nvm_slab_alloc(NvmSlab* self, uint32_t* out_block_idx) {
    // 1. 检查 self 和 out_block_idx 指针的有效性
    if (self == NULL || out_block_idx == NULL) {
        return -1; // 无效参数
    }

    // 2. 检查 cache_count 是否为 0，如果是，则尝试填充缓存
    if (self->cache_count == 0) {
        refill_cache(self);
    }

    // 3. 再次检查 cache_count 是否为 0，如果是，则说明Slab已满，无法分配
    if (self->cache_count == 0) {
        // 经过refill尝试后，缓存依然为空，说明Slab真的满了
        return -1; // 表示Slab已满
    }

    // 4. (快速路径) 从环形缓冲区的 head 取出一个块索引
    uint32_t block_idx = self->free_block_buffer[self->cache_head];
    
    // 5. 更新 cache_head (环形) 和 cache_count
    self->cache_head = (self->cache_head + 1) % SLAB_CACHE_SIZE;
    self->cache_count--;

    // 6. 更新Slab的总分配计数器
    // 注意：只有当一个块真正从缓存中被取出并交付给用户时，我们才增加总分配数
    self->allocated_block_count++;

    // 7. 将分配到的块索引存入输出参数
    *out_block_idx = block_idx;

    // 8. 返回 0 表示成功
    return 0;
}

/**
 * @brief (内部函数) 将DRAM缓存中的空闲块索引回写到位图，直到数量降至低水位线。
 *
 * 当DRAM缓存达到高水位线（满）时，此函数被调用。它会从缓存的头部
 * 不断取出块索引并回写到位图，直到缓存中的块数量减少到一个
 * “舒适”的水平（低水位线，即 SLAB_CACHE_BATCH_SIZE），从而为后续的
 * free 操作批量腾出空间。
 *
 * @param self 指向要操作的 NvmSlab 对象的指针。
 * @return 成功回写到"位图"中的块的数量。
 */
static uint32_t drain_cache(NvmSlab* self) {
    // 防御性检查：如果缓存数量本就在低水位线或以下，没有必要回写。
    // 在我们的设计中，它总是在满的时候被调用，但这个检查让函数本身更健壮。
    if (self->cache_count <= SLAB_CACHE_BATCH_SIZE) {
        return 0;
    }

    // 计算需要回写的数量，目标是让 cache_count 降到 SLAB_CACHE_BATCH_SIZE
    uint32_t count_to_drain = self->cache_count - SLAB_CACHE_BATCH_SIZE;

    uint32_t drained_count = 0;
    for (uint32_t i = 0; i < count_to_drain; ++i) {
        // 安全检查：确保我们不会从一个空的缓存中取数据
        if (self->cache_count == 0) {
            break; // 理论上不应发生，但这是好的防御性代码
        }

        // 1. 从缓存的头部取出一个块索引
        uint32_t block_idx = self->free_block_buffer[self->cache_head];
        
        // 2. 更新缓存的头部索引 (环形)
        self->cache_head = (self->cache_head + 1) % SLAB_CACHE_SIZE;
        
        // 3. 在位图中将对应的位清除为0
        CLEAR_BIT(self->bitmap, block_idx);
        
        // 4. 在循环内部就递减 cache_count，保持状态实时同步
        self->cache_count--;
        
        drained_count++;
    }

    return drained_count;
}



/**
 * @brief 将一个之前分配的块释放回指定的 Slab。
 *
 * 被释放的块将被归还到 Slab 的内部DRAM缓存中。如果DRAM缓存已满，
 * 则会触发一个 "drain" 操作，将缓存中一部分“最老”的空闲块索引
 * 回写到位图中，为新释放的块腾出空间。
 *
 * @param self 指向要操作的 NvmSlab 对象的指针。
 * @param block_idx 要释放的块的索引。
 */
void nvm_slab_free(NvmSlab* self, uint32_t block_idx) {
    // 步骤 1: 检查 self 指针的有效性，以及 block_idx 的范围 (防御性编程)
    if (self == NULL) {
        return;
    }
    if (block_idx >= self->total_block_count) {
        fprintf(stderr, "Error: Attempt to free an out-of-bounds block index (%u) for a slab with %u total blocks.\n", block_idx, self->total_block_count);
        return;
    }

    // [可选] 检查块是否已经被释放 (双重释放检查)
    if (!IS_BIT_SET(self->bitmap, block_idx)) {
        // 这个块在位图中显示是空闲的，这可能意味着双重释放。
        fprintf(stderr, "Warning: Double free detected for block index %u.\n", block_idx);
        return;
    }

    // 步骤 2: 更新总分配计数器
    // 只有当 allocated_block_count > 0 时才递减，防止因错误调用free导致下溢。
    if (self->allocated_block_count > 0) {
        self->allocated_block_count--;
    } else {
        // 如果 allocated_block_count 已经是0，却仍在调用 free，这也是一个应用层错误。
        fprintf(stderr, "Warning: nvm_slab_free called on an already empty slab.\n");
    }

    // 步骤 3: 检查 cache 是否已满，如果满，则调用 drain_cache 为新块腾出空间
    if (self->cache_count >= SLAB_CACHE_SIZE) {
        // 调用我们优化后的 drain_cache 函数。
        // 它会将缓存数量从 SLAB_CACHE_SIZE (64) 降低到 SLAB_CACHE_BATCH_SIZE (32)。
        drain_cache(self);
    }
    
    // 步骤 4: (快速路径) 将被释放的块索引放入环形缓冲区的尾部
    self->free_block_buffer[self->cache_tail] = block_idx;
    
    // 步骤 5: 更新环形缓冲区的尾部索引和当前数量
    self->cache_tail = (self->cache_tail + 1) % SLAB_CACHE_SIZE;
    self->cache_count++;
}


bool nvm_slab_is_full(const NvmSlab* self) {
    // 步骤 1: 检查 self 指针的有效性（防御性编程）
    if (self == NULL) {
        return false;
    }

    // 步骤 2: 比较已分配块数和总块数
    return self->allocated_block_count == self->total_block_count;
}


bool nvm_slab_is_empty(const NvmSlab* self) {
    // 步骤 1: 检查 self 指针的有效性
    if (self == NULL) {
        return true;
    }

    // 步骤 2: 检查已分配块数是否为 0
    return self->allocated_block_count == 0;
}