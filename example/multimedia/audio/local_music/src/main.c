/*
 * Single-threaded FFmpeg AAC/MP3 decoder performance test
 * Flow: decode all frames -> save PCM -> play audio
 * Measures decode time using DWT cycle counter
 */

#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include <stdio.h>
#include <string.h>
#include <rtdevice.h>
#include <core_cm33.h>

#if RT_USING_DFS
    #include "dfs_file.h"
    #include "dfs_posix.h"
    #include <fcntl.h>
#endif

#include "drv_flash.h"
#include "audio_server.h"

/* PSRAM - use ffmpeg_mem.c's existing PSRAM heap */

/* FFmpeg headers */
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"

#define DBG_TAG    "ffmpeg_perf"
#define DBG_LVL    LOG_LVL_INFO
#include <log.h>

/* File system */
#ifndef FS_REGION_START_ADDR
    #error "Need to define file system start address!"
#endif
#define FS_ROOT "root"
#define MUSIC_FILE "/iphone.aac"

/* DWT cycle counter */
#define CPU_FREQ_HZ    200000000  /* 200 MHz */

/* PCM buffer - use PSRAM */
#define MAX_PCM_BUFFER_SIZE  (1024 * 1024)  /* 1MB max */

/* Use ffmpeg_mem.c's PSRAM allocator (avoids duplicate heap init) */
extern void *ffmpeg_alloc(size_t size);
extern void ffmpeg_free(void *ptr);

/* Performance stats */
struct perf_stats
{
    uint32_t total_decode_cycles;
    uint32_t max_decode_cycles;
    uint32_t min_decode_cycles;
    uint32_t frame_count;
    uint32_t error_count;
    uint32_t start_tick;      /* Start tick for CPU calculation */
    uint32_t end_tick;        /* End tick for CPU calculation */
};

static struct perf_stats g_perf;
static AVFormatContext *g_fmt_ctx = NULL;
static AVCodecContext *g_dec_ctx = NULL;
static int g_audio_stream_idx = -1;

/* PCM buffer */
static int16_t *g_pcm_buf = NULL;
static uint32_t g_pcm_size = 0;      /* bytes */
static uint32_t g_pcm_cap = 0;       /* capacity in bytes */

/* =====================================================================
 *  Filesystem init
 * ===================================================================== */
static int mnt_init(void)
{
    register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE, FS_ROOT);
    if (dfs_mount(FS_ROOT, "/", "elm", 0, 0) == 0)
    {
        rt_kprintf("[INIT] mount fs success\n");
    }
    else
    {
        rt_kprintf("[INIT] mount fs fail, try mkfs\n");
        if (dfs_mkfs("elm", FS_ROOT) == 0)
        {
            if (dfs_mount(FS_ROOT, "/", "elm", 0, 0) == 0)
                rt_kprintf("[INIT] mount fs success\n");
        }
    }
    return RT_EOK;
}
INIT_ENV_EXPORT(mnt_init);

/* =====================================================================
 *  DWT Performance Measurement
 * ===================================================================== */
static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static inline uint32_t dwt_get_cycles(void)
{
    return DWT->CYCCNT;
}

static void perf_reset(void)
{
    memset(&g_perf, 0, sizeof(g_perf));
}

static void perf_start(uint32_t *start)
{
    *start = dwt_get_cycles();
}

static void perf_stop(uint32_t start)
{
    uint32_t cycles = dwt_get_cycles() - start;
    g_perf.total_decode_cycles += cycles;
    g_perf.frame_count++;

    if (cycles > g_perf.max_decode_cycles)
        g_perf.max_decode_cycles = cycles;

    if (g_perf.min_decode_cycles == 0 || cycles < g_perf.min_decode_cycles)
        g_perf.min_decode_cycles = cycles;
}

static void perf_report(void)
{
    rt_kprintf("\n========== Decode Performance ==========\n");
    if (g_perf.frame_count == 0)
    {
        rt_kprintf("No frames decoded\n");
        return;
    }

    uint32_t avg = g_perf.total_decode_cycles / g_perf.frame_count;
    float avg_ms = (float)avg / (CPU_FREQ_HZ / 1000);
    float total_decode_ms = (float)g_perf.total_decode_cycles / (CPU_FREQ_HZ / 1000);

    /* Calculate total elapsed time */
    uint32_t elapsed_cycles = g_perf.end_tick - g_perf.start_tick;
    float total_elapsed_ms = (float)elapsed_cycles / (CPU_FREQ_HZ / 1000);

    /* CPU usage = decode_time / elapsed_time * 100% */
    float cpu_usage = 0;
    if (elapsed_cycles > 0)
        cpu_usage = (float)g_perf.total_decode_cycles * 100.0f / elapsed_cycles;

    rt_kprintf("Frames:         %d\n", g_perf.frame_count);
    rt_kprintf("Avg decode:     %d cycles (%.2f ms)\n", avg, avg_ms);
    rt_kprintf("Max decode:     %d cycles (%.2f ms)\n", g_perf.max_decode_cycles,
               (float)g_perf.max_decode_cycles / (CPU_FREQ_HZ / 1000));
    rt_kprintf("Min decode:     %d cycles (%.2f ms)\n", g_perf.min_decode_cycles,
               (float)g_perf.min_decode_cycles / (CPU_FREQ_HZ / 1000));
    rt_kprintf("Total decode:   %.2f ms\n", total_decode_ms);
    rt_kprintf("Total elapsed:  %.2f ms\n", total_elapsed_ms);
    rt_kprintf("CPU usage:      %.1f%% (DWT)\n", cpu_usage);
    rt_kprintf("PCM size:       %d bytes\n", g_pcm_size);
    rt_kprintf("Errors:         %d\n", g_perf.error_count);
    rt_kprintf("========================================\n\n");
}

/* =====================================================================
 *  Audio Output
 * ===================================================================== */
static audio_client_t g_audio_client = NULL;

static int audio_write_callback(audio_server_callback_cmt_t cmd, void *userdata, uint32_t reserved)
{
    return 0;
}

static int audio_output_open(int sample_rate, int channels, int bits_per_sample)
{
    audio_parameter_t pa = {0};
    pa.write_samplerate = sample_rate;
    pa.write_channnel_num = channels;
    pa.write_bits_per_sample = bits_per_sample;
    pa.write_cache_size = 8192;  /* Larger cache for smoother playback */
    pa.read_samplerate = 0;
    pa.read_bits_per_sample = 0;
    pa.read_channnel_num = 0;
    pa.read_cache_size = 0;

    g_audio_client = audio_open(AUDIO_TYPE_LOCAL_MUSIC, AUDIO_TX, &pa,
                                audio_write_callback, NULL);
    if (!g_audio_client)
    {
        rt_kprintf("[AUDIO] open failed\n");
        return -1;
    }
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 15);
    return 0;
}

static int audio_output_write(uint8_t *data, int size)
{
    if (!g_audio_client)
        return -1;
    return audio_write(g_audio_client, data, size);
}

static void audio_output_close(void)
{
    if (g_audio_client)
    {
        audio_close(g_audio_client);
        g_audio_client = NULL;
    }
}

/* =====================================================================
 *  Save PCM to file
 * ===================================================================== */
static int save_pcm_to_file(const char *filepath)
{
    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
    {
        rt_kprintf("[SAVE] open %s failed\n", filepath);
        return -1;
    }

    int written = write(fd, g_pcm_buf, g_pcm_size);
    close(fd);

    rt_kprintf("[SAVE] saved %d bytes to %s\n", written, filepath);
    return 0;
}

/* =====================================================================
 *  Raw FFmpeg Audio Decode (single-threaded, old API)
 * ===================================================================== */
static int ffmpeg_open_file(const char *filepath)
{
    int ret;
    AVCodec *codec;
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;

    rt_kprintf("[FFMPEG] Opening %s\n", filepath);

    /* Check if file exists */
    int fd = open(filepath, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("[FFMPEG] File not found: %s\n", filepath);
        return -1;
    }
    close(fd);

    /* Open input file with options */
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "1048576", 0);  /* 1MB */

    ret = avformat_open_input(&fmt_ctx, filepath, NULL, &opts);
    av_dict_free(&opts);

    if (ret < 0)
    {
        char errbuf[64];
        av_strerror(ret, errbuf, sizeof(errbuf));
        rt_kprintf("[FFMPEG] avformat_open_input failed: %d (%s)\n", ret, errbuf);
        return ret;
    }

    /* Find stream info */
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0)
    {
        rt_kprintf("[FFMPEG] avformat_find_stream_info failed: %d\n", ret);
        avformat_close_input(&fmt_ctx);
        return ret;
    }

    /* Find audio stream index */
    g_audio_stream_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++)
    {
        if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            g_audio_stream_idx = i;
            break;
        }
    }

    if (g_audio_stream_idx < 0)
    {
        rt_kprintf("[FFMPEG] no audio stream found\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    /* Get codec context from stream */
    dec_ctx = fmt_ctx->streams[g_audio_stream_idx]->codec;

    /* Find decoder based on codec_id from stream */
    codec = avcodec_find_decoder(dec_ctx->codec_id);
    if (!codec)
    {
        rt_kprintf("[FFMPEG] decoder not found for codec_id=%d\n", dec_ctx->codec_id);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    rt_kprintf("[FFMPEG] audio stream idx=%d, codec_id=%d\n", g_audio_stream_idx, dec_ctx->codec_id);

    /* Open decoder */
    ret = avcodec_open2(dec_ctx, codec, NULL);
    if (ret < 0)
    {
        rt_kprintf("[FFMPEG] avcodec_open2 failed: %d\n", ret);
        avformat_close_input(&fmt_ctx);
        return ret;
    }

    /* Print info */
    rt_kprintf("[FFMPEG] sample_rate=%d, channels=%d, format=%d\n",
               dec_ctx->sample_rate, dec_ctx->channels, dec_ctx->sample_fmt);
    if (dec_ctx->codec->name)
        rt_kprintf("[FFMPEG] codec: %s\n", dec_ctx->codec->name);

    g_fmt_ctx = fmt_ctx;
    g_dec_ctx = dec_ctx;
    return 0;
}

static void ffmpeg_close_file(void)
{
    if (g_dec_ctx)
    {
        avcodec_close(g_dec_ctx);
        g_dec_ctx = NULL;
    }
    if (g_fmt_ctx)
    {
        avformat_close_input(&g_fmt_ctx);
        g_fmt_ctx = NULL;
    }
    g_audio_stream_idx = -1;
}

/**
 * @brief Decode all frames and store in PCM buffer (using PSRAM)
 */
static int ffmpeg_decode_all(void)
{
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (!pkt || !frame)
    {
        rt_kprintf("[FFMPEG] alloc packet/frame failed\n");
        return -1;
    }

    /* Allocate PCM buffer from PSRAM */
    g_pcm_cap = MAX_PCM_BUFFER_SIZE;
    g_pcm_buf = (int16_t *)ffmpeg_alloc(g_pcm_cap);
    if (!g_pcm_buf)
    {
        rt_kprintf("[FFMPEG] PSRAM malloc failed (%d bytes), try system heap\n", g_pcm_cap);
        g_pcm_buf = (int16_t *)rt_malloc(g_pcm_cap);
    }
    if (!g_pcm_buf)
    {
        rt_kprintf("[FFMPEG] malloc PCM buffer failed\n");
        av_packet_free(&pkt);
        av_frame_free(&frame);
        return -1;
    }
    g_pcm_size = 0;

    perf_reset();
    g_perf.start_tick = dwt_get_cycles();  /* Record start time */
    rt_kprintf("[FFMPEG] Decoding all frames...\n");

    int ret;
    int got_frame;
    uint32_t decode_start;

    while (av_read_frame(g_fmt_ctx, pkt) >= 0)
    {
        if (pkt->stream_index != g_audio_stream_idx)
        {
            av_packet_unref(pkt);
            continue;
        }

        /* Measure decode time */
        perf_start(&decode_start);

        /* Decode audio packet */
        got_frame = 0;
        ret = avcodec_decode_audio4(g_dec_ctx, frame, &got_frame, pkt);

        if (ret < 0)
        {
            rt_kprintf("[FFMPEG] decode error: %d\n", ret);
            g_perf.error_count++;
            av_packet_unref(pkt);
            continue;
        }

        perf_stop(decode_start);

        if (got_frame)
        {
            int samples = frame->nb_samples;
            int channels = g_dec_ctx->channels;

            if (g_dec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP)
            {
                /* Float planar -> int16 interleaved */
                int bytes_needed = samples * channels * 2;
                if (g_pcm_size + bytes_needed <= g_pcm_cap)
                {
                    int16_t *pcm_ptr = (int16_t *)((uint8_t *)g_pcm_buf + g_pcm_size);
                    for (int s = 0; s < samples; s++)
                    {
                        for (int c = 0; c < channels; c++)
                        {
                            float val = ((float *)frame->data[c])[s];
                            val = val < -1.0f ? -1.0f : (val > 1.0f ? 1.0f : val);
                            pcm_ptr[s * channels + c] = (int16_t)(val * 32767);
                        }
                    }
                    g_pcm_size += bytes_needed;
                }
                else
                {
                    rt_kprintf("[FFMPEG] PCM buffer full at %d bytes\n", g_pcm_size);
                    break;
                }
            }
            else if (g_dec_ctx->sample_fmt == AV_SAMPLE_FMT_S16)
            {
                /* Already int16 */
                int bytes_needed = samples * channels * 2;
                if (g_pcm_size + bytes_needed <= g_pcm_cap)
                {
                    memcpy((uint8_t *)g_pcm_buf + g_pcm_size, frame->data[0], bytes_needed);
                    g_pcm_size += bytes_needed;
                }
                else
                {
                    rt_kprintf("[FFMPEG] PCM buffer full at %d bytes\n", g_pcm_size);
                    break;
                }
            }
        }

        av_packet_unref(pkt);
    }

    /* Flush decoder */
    pkt->data = NULL;
    pkt->size = 0;
    do
    {
        got_frame = 0;
        ret = avcodec_decode_audio4(g_dec_ctx, frame, &got_frame, pkt);
        if (got_frame)
        {
            int samples = frame->nb_samples;
            int channels = g_dec_ctx->channels;

            if (g_dec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP)
            {
                int bytes_needed = samples * channels * 2;
                if (g_pcm_size + bytes_needed <= g_pcm_cap)
                {
                    int16_t *pcm_ptr = (int16_t *)((uint8_t *)g_pcm_buf + g_pcm_size);
                    for (int s = 0; s < samples; s++)
                    {
                        for (int c = 0; c < channels; c++)
                        {
                            float val = ((float *)frame->data[c])[s];
                            val = val < -1.0f ? -1.0f : (val > 1.0f ? 1.0f : val);
                            pcm_ptr[s * channels + c] = (int16_t)(val * 32767);
                        }
                    }
                    g_pcm_size += bytes_needed;
                }
            }
            else if (g_dec_ctx->sample_fmt == AV_SAMPLE_FMT_S16)
            {
                int bytes_needed = samples * channels * 2;
                if (g_pcm_size + bytes_needed <= g_pcm_cap)
                {
                    memcpy((uint8_t *)g_pcm_buf + g_pcm_size, frame->data[0], bytes_needed);
                    g_pcm_size += bytes_needed;
                }
            }
        }
    }
    while (got_frame);

    g_perf.end_tick = dwt_get_cycles();  /* Record end time */

    av_packet_free(&pkt);
    av_frame_free(&frame);

    rt_kprintf("[FFMPEG] Decode complete: %d bytes PCM\n", g_pcm_size);
    return 0;
}

/**
 * @brief Play PCM buffer through audio output
 */
static void play_pcm_buffer(void)
{
    if (!g_pcm_buf || g_pcm_size == 0)
    {
        rt_kprintf("[PLAY] no PCM data to play\n");
        return;
    }

    rt_kprintf("[PLAY] Playing %d bytes PCM...\n", g_pcm_size);

    /* Open audio output */
    if (audio_output_open(g_dec_ctx->sample_rate, g_dec_ctx->channels, 16) < 0)
        return;

    /* Write PCM data in chunks */
    int chunk_size = 4096;
    uint32_t offset = 0;

    while (offset < g_pcm_size)
    {
        int size = (g_pcm_size - offset > chunk_size) ? chunk_size : (g_pcm_size - offset);
        audio_output_write((uint8_t *)g_pcm_buf + offset, size);
        offset += size;
        rt_thread_mdelay(10);  /* Pace the output */
    }

    /* Wait for audio to finish playing */
    rt_thread_mdelay(500);

    audio_output_close();
    rt_kprintf("[PLAY] Done\n");
}

/* =====================================================================
 *  Shell commands
 * ===================================================================== */

/**
 * @brief play [file]
 *   Decode and play audio file (default: /iphone.aac)
 */
static int cmd_play(int argc, char *argv[])
{
    const char *filepath = (argc > 1) ? argv[1] : MUSIC_FILE;
    char path_buf[128];

    /* Auto-add leading slash if missing */
    if (filepath[0] != '/')
    {
        snprintf(path_buf, sizeof(path_buf), "/%s", filepath);
        filepath = path_buf;
    }

    /* Step 1: Open file and decode all frames */
    if (ffmpeg_open_file(filepath) < 0)
        return -1;

    if (ffmpeg_decode_all() < 0)
    {
        ffmpeg_close_file();
        return -1;
    }

    /* Step 2: Save PCM to file */
    save_pcm_to_file("/decoded.pcm");

    /* Step 3: Print performance */
    perf_report();

    /* Step 4: Play the decoded audio */
    play_pcm_buffer();

    /* Cleanup */
    if (g_pcm_buf)
    {
        ffmpeg_free(g_pcm_buf);
        g_pcm_buf = NULL;
    }
    g_pcm_size = 0;

    ffmpeg_close_file();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_play, play, Decode and play audio file);

/**
 * @brief stop
 *   Stop playback
 */
static int cmd_stop(int argc, char *argv[])
{
    audio_output_close();
    if (g_pcm_buf)
    {
        ffmpeg_free(g_pcm_buf);
        g_pcm_buf = NULL;
    }
    g_pcm_size = 0;
    ffmpeg_close_file();
    rt_kprintf("[PLAY] stopped\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_stop, stop, Stop playback);

/* =====================================================================
 *  Main
 * ===================================================================== */
int main(void)
{
    dwt_init();

    /* Register all FFmpeg formats, codecs, and protocols */
    av_register_all();

    rt_kprintf("\n========================================\n");
    rt_kprintf("  FFmpeg Audio Decode Performance Test\n");
    rt_kprintf("  Single-threaded, raw FFmpeg API\n");
    rt_kprintf("========================================\n");
    rt_kprintf("Commands:\n");
    rt_kprintf("  play [file]  - decode and play (default: /iphone.aac)\n");
    rt_kprintf("  stop         - stop playback\n");
    rt_kprintf("========================================\n\n");

    while (1)
    {
        rt_thread_mdelay(1000);
    }
    return 0;
}