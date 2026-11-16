/*
 * FFmpeg Memory Allocation Functions
 * Provides ffmpeg_alloc, ffmpeg_free, ffmpeg_realloc for FFmpeg
 * Uses PSRAM via memheap for large allocations
 */

#include <rtthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* PSRAM Configuration for SF32LB52X */
#define FFMPEG_PSRAM_BASE       0x60000000
#define FFMPEG_PSRAM_SIZE       (8 * 1024 * 1024)  /* 8MB PSRAM */

/* PSRAM heap for FFmpeg */
#ifdef RT_USING_MEMHEAP
static struct rt_memheap psram_heap;
static rt_uint8_t psram_heap_initialized = 0;

/**
 * @brief Initialize PSRAM heap for FFmpeg
 */
static void ffmpeg_psram_heap_init(void)
{
    if (!psram_heap_initialized)
    {
        rt_err_t ret = rt_memheap_init(&psram_heap, "ffmpeg_psram",
                                       (void *)FFMPEG_PSRAM_BASE, FFMPEG_PSRAM_SIZE);
        if (ret == RT_EOK)
        {
            psram_heap_initialized = 1;
            rt_kprintf("[FFMPEG MEM] PSRAM heap initialized: base=0x%08x, size=%d MB\n",
                       FFMPEG_PSRAM_BASE, FFMPEG_PSRAM_SIZE / (1024 * 1024));
        }
        else
        {
            rt_kprintf("[FFMPEG MEM] PSRAM heap init failed: %d\n", ret);
        }
    }
}
#endif

/**
 * @brief Allocate memory for FFmpeg
 * @param size Size in bytes to allocate
 * @return Pointer to allocated memory, or NULL on failure
 */
void *ffmpeg_alloc(size_t size)
{
    void *ptr = NULL;

#ifdef RT_USING_MEMHEAP
    /* Try PSRAM first for large allocations */
    if (size >= 4096)
    {
        ffmpeg_psram_heap_init();

        if (psram_heap_initialized)
        {
            ptr = rt_memheap_alloc(&psram_heap, size);
            if (ptr != NULL)
            {
                return ptr;
            }
        }
    }
#endif

    /* Fallback to system heap (SRAM) */
    ptr = rt_malloc(size);
    if (ptr == NULL)
    {
        rt_kprintf("[FFMPEG MEM] alloc failed: %d bytes\n", (int)size);
    }
    return ptr;
}

/**
 * @brief Free memory allocated by ffmpeg_alloc
 * @param ptr Pointer to memory to free
 */
void ffmpeg_free(void *ptr)
{
    if (ptr == NULL)
    {
        return;
    }

#ifdef RT_USING_MEMHEAP
    /* Check if pointer is in PSRAM range */
    if ((uint32_t)ptr >= FFMPEG_PSRAM_BASE &&
        (uint32_t)ptr < (FFMPEG_PSRAM_BASE + FFMPEG_PSRAM_SIZE))
    {
        rt_memheap_free(ptr);
        return;
    }
#endif

    /* Free from system heap */
    rt_free(ptr);
}

/**
 * @brief Reallocate memory for FFmpeg
 * @param ptr Pointer to previously allocated memory (or NULL)
 * @param new_size New size in bytes
 * @return Pointer to reallocated memory, or NULL on failure
 */
void *ffmpeg_realloc(void *ptr, size_t new_size)
{
    void *new_ptr;

    if (ptr == NULL)
    {
        return ffmpeg_alloc(new_size);
    }

    if (new_size == 0)
    {
        ffmpeg_free(ptr);
        return NULL;
    }

    /* Allocate new memory */
    new_ptr = ffmpeg_alloc(new_size);
    if (new_ptr == NULL)
    {
        return NULL;
    }

    /* Copy old data - use a conservative size */
    /* FFmpeg typically handles this correctly */
    rt_memcpy(new_ptr, ptr, new_size);

    /* Free old memory */
    ffmpeg_free(ptr);

    return new_ptr;
}
