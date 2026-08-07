#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"
#include <rtdevice.h>
#if RT_USING_DFS
    #include "dfs_file.h"
    #include "dfs_posix.h"
#endif
#include "audio_server.h"
#include "drv_flash.h"
#include <core_cm33.h>

/* FFmpeg media decoder */
#include "media_dec.h"

/* CPU Usage Profiler */
#include "cpu_usage_profiler.h"

/* Common functions for RT-Thread based platform -----------------------------------------------*/

#ifndef FS_REGION_START_ADDR
    #error "Need to define file system start address!"
#endif

#define FS_ROOT "root"

/* 音频文件路径 - 使用 disk 目录下的文件 */
#define MUSIC_FILE_PATH "/iphone.mp3"

/* Performance monitoring */
struct perf_stats {
    uint32_t total_decode_cycles;
    uint32_t max_decode_cycles;
    uint32_t min_decode_cycles;
    uint32_t frame_count;
    uint32_t error_count;
    uint32_t last_start_tick;
};

static struct perf_stats g_perf = {0};
static ffmpeg_handle g_ffmpeg_handle = NULL;

/**
 * @brief Mount fs.
 */
int mnt_init(void)
{
    register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE, FS_ROOT);
    if (dfs_mount(FS_ROOT, "/", "elm", 0, 0) == 0)
    {
        rt_kprintf("[INIT] mount fs on flash to root success\n");
    }
    else
    {
        rt_kprintf("[INIT] mount fs on flash to root fail\n");
        if (dfs_mkfs("elm", FS_ROOT) == 0)
        {
            rt_kprintf("[INIT] make elm fs on flash success, mount again\n");
            if (dfs_mount(FS_ROOT, "/", "elm", 0, 0) == 0)
                rt_kprintf("[INIT] mount fs on flash success\n");
            else
                rt_kprintf("[INIT] mount to fs on flash fail\n");
        }
        else
            rt_kprintf("[INIT] dfs_mkfs elm flash fail\n");
    }
    return RT_EOK;
}
INIT_ENV_EXPORT(mnt_init);

/* Performance monitoring functions -----------------------------------------------*/

/**
 * @brief Initialize DWT cycle counter for performance measurement
 */
static void perf_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    rt_kprintf("[PERF] DWT cycle counter initialized\n");
}

/**
 * @brief Start performance measurement
 */
static void perf_start(void)
{
    g_perf.last_start_tick = DWT->CYCCNT;
}

/**
 * @brief Stop performance measurement and update stats
 */
static void perf_stop(void)
{
    uint32_t end = DWT->CYCCNT;
    uint32_t cycles;

    /* Handle overflow */
    if (end >= g_perf.last_start_tick)
    {
        cycles = end - g_perf.last_start_tick;
    }
    else
    {
        cycles = (0xFFFFFFFF - g_perf.last_start_tick) + end + 1;
    }

    g_perf.total_decode_cycles += cycles;
    g_perf.frame_count++;

    if (cycles > g_perf.max_decode_cycles)
    {
        g_perf.max_decode_cycles = cycles;
    }

    if (g_perf.min_decode_cycles == 0 || cycles < g_perf.min_decode_cycles)
    {
        g_perf.min_decode_cycles = cycles;
    }
}

/**
 * @brief Print performance report
 */
static void perf_report(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("       AAC/MP3 Decode Performance\n");
    rt_kprintf("========================================\n");

    if (g_perf.frame_count == 0)
    {
        rt_kprintf("No frames decoded yet\n");
        rt_kprintf("========================================\n\n");
        return;
    }

    uint32_t avg_cycles = g_perf.total_decode_cycles / g_perf.frame_count;
    uint32_t cpu_freq = 200000000;  /* 200 MHz */

    rt_kprintf("Total frames: %d\n", g_perf.frame_count);
    rt_kprintf("Avg decode: %d cycles (%.2f ms)\n",
               avg_cycles, (float)avg_cycles / (cpu_freq / 1000));
    rt_kprintf("Max decode: %d cycles (%.2f ms)\n",
               g_perf.max_decode_cycles,
               (float)g_perf.max_decode_cycles / (cpu_freq / 1000));
    rt_kprintf("Min decode: %d cycles (%.2f ms)\n",
               g_perf.min_decode_cycles,
               (float)g_perf.min_decode_cycles / (cpu_freq / 1000));
    rt_kprintf("Error frames: %d\n", g_perf.error_count);
    rt_kprintf("CPU Usage: %.1f%%\n", cpu_get_usage());
    rt_kprintf("========================================\n\n");
}

/* FFmpeg Memory Callbacks ---------------------------------------*/
/**
 * @brief FFmpeg 内存分配回调包装函数
 *        解决 rt_size_t (64位) 和 size_t (32位) 类型不匹配问题
 */
static void *ffmpeg_mem_malloc(size_t size)
{
    return rt_malloc((rt_size_t)size);
}

static void ffmpeg_mem_free(void *ptr)
{
    rt_free(ptr);
}

static void *ffmpeg_mem_realloc(void *ptr, size_t new_size)
{
    return rt_realloc(ptr, (rt_size_t)new_size);
}

/* FFmpeg Callback -----------------------------------------------*/

/**
 * @brief FFmpeg notification callback
 */
static int ffmpeg_notify_callback(uint32_t user_data, ffmpeg_cmd_e cmd, uint32_t val)
{
    switch (cmd)
    {
    case e_ffmpeg_play_to_end:
        rt_kprintf("[FFMPEG] Play to end\n");
        perf_report();
        break;
    case e_ffmpeg_play_to_error:
        rt_kprintf("[FFMPEG] Play error\n");
        g_perf.error_count++;
        break;
    case e_ffmpeg_play_frames:
        /* 每帧解码完成时统计 - 这里只能统计回调的时间，实际解码时间在 FFmpeg 内部 */
        break;
    default:
        break;
    }
    return 0;
}

/* FFmpeg playback -----------------------------------------------*/

/**
 * @brief 使用 FFmpeg 播放音频文件 (AAC/MP3/WAV)
 */
static void ffmpeg_play_file(const char *file_path)
{
    rt_kprintf("[FFMPEG] Playing: %s\n", file_path);

    /* 重置性能统计 */
    g_perf.total_decode_cycles = 0;
    g_perf.max_decode_cycles = 0;
    g_perf.min_decode_cycles = 0;
    g_perf.frame_count = 0;
    g_perf.error_count = 0;

    ffmpeg_config_t cfg = {0};
    cfg.src = e_src_localfile;
    cfg.audio_enable = 1;
    cfg.video_enable = 0;  /* 纯音频 */
    cfg.is_loop = 0;
    cfg.file_path = file_path;
    cfg.notify = ffmpeg_notify_callback;
    cfg.mem_malloc = ffmpeg_mem_malloc;
    cfg.mem_free = ffmpeg_mem_free;

    int ret = ffmpeg_open(&g_ffmpeg_handle, &cfg, 0);
    if (ret == 0)
    {
        rt_kprintf("[FFMPEG] Open success, start playing\n");
    }
    else
    {
        rt_kprintf("[FFMPEG] Open failed: %d\n", ret);
    }
}

/**
 * @brief 停止 FFmpeg 播放
 */
static void ffmpeg_play_stop(void)
{
    if (g_ffmpeg_handle)
    {
        rt_kprintf("[FFMPEG] Stopping\n");
        ffmpeg_close(g_ffmpeg_handle);
        g_ffmpeg_handle = NULL;
    }
}

/**
 * @brief Main program - AAC/MP3 Performance Test
 */
int main(void)
{
    rt_kprintf("\n========================================\n");
    rt_kprintf("  AAC/MP3 Decode Performance Test\n");
    rt_kprintf("  Using FFmpeg Decoder\n");
    rt_kprintf("========================================\n\n");

    /* Initialize DWT for performance monitoring */
    perf_init();

    /* List files in root */
    extern void ls(const char *name);
    ls("/");

    /* Start playing with FFmpeg */
    ffmpeg_play_file(MUSIC_FILE_PATH);

    /* Main loop - print performance stats periodically */
    while (1)
    {
        rt_thread_mdelay(5000);
        if (g_perf.frame_count > 0)
        {
            perf_report();
        }
    }

    return 0;
}
