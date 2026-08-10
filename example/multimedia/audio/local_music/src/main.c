/*
 * FFmpeg AAC Audio Decode Performance Test
 * Streaming model: decode frame -> audio_write -> next frame
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

/* PCM conversion buffer (per frame, in SRAM for speed) */
#define PCM_FRAME_BUF_SIZE  (4096 * 4)

/* Use ffmpeg_mem.c's PSRAM allocator */
extern void *ffmpeg_alloc(size_t size);
extern void ffmpeg_free(void *ptr);

/* =====================================================================
 *  Performance stats
 * ===================================================================== */
struct perf_stats
{
    uint32_t total_decode_cycles;
    uint32_t max_decode_cycles;
    uint32_t min_decode_cycles;
    uint32_t frame_count;
    uint32_t error_count;
    uint32_t total_pcm_bytes;
    uint32_t start_tick;
    uint32_t end_tick;
};

static struct perf_stats g_perf;

static void perf_reset(void)
{
    memset(&g_perf, 0, sizeof(g_perf));
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
    uint32_t elapsed_cycles = g_perf.end_tick - g_perf.start_tick;
    float total_elapsed_ms = (float)elapsed_cycles / (CPU_FREQ_HZ / 1000);
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
    rt_kprintf("CPU usage:      %.1f%%\n", cpu_usage);
    rt_kprintf("PCM output:     %d bytes\n", g_perf.total_pcm_bytes);
    rt_kprintf("Errors:         %d\n", g_perf.error_count);
    rt_kprintf("========================================\n\n");
}

/* =====================================================================
 *  DWT
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
    pa.write_cache_size = 32768;  /* 32KB cache for smoother playback */
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
    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 4);
    return 0;
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
 *  Streaming decode + play (like media_player_server.c)
 *
 *  Flow: av_read_frame -> decode -> convert -> audio_write
 *  audio_write blocks when cache full, pacing the decode naturally.
 * ===================================================================== */
static volatile int g_stop_flag = 0;

static int cmd_play(int argc, char *argv[])
{
    const char *filepath = (argc > 1) ? argv[1] : MUSIC_FILE;
    char path_buf[128];
    int ret;

    /* Auto-add leading slash if missing */
    if (filepath[0] != '/')
    {
        snprintf(path_buf, sizeof(path_buf), "/%s", filepath);
        filepath = path_buf;
    }

    g_stop_flag = 0;

    rt_kprintf("[FFMPEG] Playing: %s\n", filepath);

    /* --- Open input --- */
    AVFormatContext *fmt_ctx = NULL;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "1048576", 0);

    ret = avformat_open_input(&fmt_ctx, filepath, NULL, &opts);
    av_dict_free(&opts);
    if (ret < 0)
    {
        char errbuf[64];
        av_strerror(ret, errbuf, sizeof(errbuf));
        rt_kprintf("[FFMPEG] avformat_open_input failed: %d (%s)\n", ret, errbuf);
        return -1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0)
    {
        rt_kprintf("[FFMPEG] avformat_find_stream_info failed: %d\n", ret);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    /* Find audio stream */
    int audio_idx = -1;
    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++)
    {
        if (fmt_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            audio_idx = i;
            break;
        }
    }
    if (audio_idx < 0)
    {
        rt_kprintf("[FFMPEG] no audio stream\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    /* Open decoder */
    AVCodecContext *dec_ctx = fmt_ctx->streams[audio_idx]->codec;
    AVCodec *codec = avcodec_find_decoder(dec_ctx->codec_id);
    if (!codec)
    {
        rt_kprintf("[FFMPEG] decoder not found\n");
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    ret = avcodec_open2(dec_ctx, codec, NULL);
    if (ret < 0)
    {
        rt_kprintf("[FFMPEG] avcodec_open2 failed: %d\n", ret);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    rt_kprintf("[FFMPEG] codec=%s, rate=%d, ch=%d, fmt=%d\n",
               dec_ctx->codec->name ? dec_ctx->codec->name : "?",
               dec_ctx->sample_rate, dec_ctx->channels, dec_ctx->sample_fmt);

    /* Open audio output */
    if (audio_output_open(dec_ctx->sample_rate, dec_ctx->channels, 16) < 0)
    {
        avcodec_close(dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    /* --- Streaming decode loop --- */
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int16_t *pcm_buf = (int16_t *)rt_malloc(PCM_FRAME_BUF_SIZE);

    if (!pkt || !frame || !pcm_buf)
    {
        rt_kprintf("[FFMPEG] alloc failed\n");
        if (pcm_buf) rt_free(pcm_buf);
        if (pkt) av_packet_free(&pkt);
        if (frame) av_frame_free(&frame);
        audio_output_close();
        avcodec_close(dec_ctx);
        avformat_close_input(&fmt_ctx);
        return -1;
    }

    perf_reset();
    g_perf.start_tick = dwt_get_cycles();

    rt_kprintf("[FFMPEG] Streaming decode+play...\n");

    while (!g_stop_flag && av_read_frame(fmt_ctx, pkt) >= 0)
    {
        if (pkt->stream_index != audio_idx)
        {
            av_packet_unref(pkt);
            continue;
        }

        /* Decode with timing */
        uint32_t t0 = dwt_get_cycles();
        int got_frame = 0;
        ret = avcodec_decode_audio4(dec_ctx, frame, &got_frame, pkt);
        uint32_t t1 = dwt_get_cycles();

        if (ret < 0)
        {
            g_perf.error_count++;
            av_packet_unref(pkt);
            continue;
        }

        /* Accumulate decode stats */
        uint32_t cycles = t1 - t0;
        g_perf.total_decode_cycles += cycles;
        g_perf.frame_count++;
        if (cycles > g_perf.max_decode_cycles)
            g_perf.max_decode_cycles = cycles;
        if (g_perf.min_decode_cycles == 0 || cycles < g_perf.min_decode_cycles)
            g_perf.min_decode_cycles = cycles;

        if (got_frame)
        {
            int samples = frame->nb_samples;
            int channels = dec_ctx->channels;
            int bytes_needed = samples * channels * 2;

            if (bytes_needed > PCM_FRAME_BUF_SIZE)
            {
                rt_kprintf("[FFMPEG] frame too large: %d bytes\n", bytes_needed);
                av_packet_unref(pkt);
                continue;
            }

            /* Convert to int16 interleaved */
            if (dec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP)
            {
                for (int s = 0; s < samples; s++)
                {
                    for (int c = 0; c < channels; c++)
                    {
                        float val = ((float *)frame->data[c])[s];
                        val = val < -1.0f ? -1.0f : (val > 1.0f ? 1.0f : val);
                        pcm_buf[s * channels + c] = (int16_t)(val * 32767);
                    }
                }
            }
            else if (dec_ctx->sample_fmt == AV_SAMPLE_FMT_S16)
            {
                memcpy(pcm_buf, frame->data[0], bytes_needed);
            }
            else
            {
                /* Unsupported format, skip */
                av_packet_unref(pkt);
                continue;
            }

            /* Write to audio output (blocks when cache full, pacing decode) */
            {
                uint8_t *write_ptr = (uint8_t *)pcm_buf;
                int write_left = bytes_needed;
                while (write_left > 0 && !g_stop_flag)
                {
                    int written = audio_write(g_audio_client, write_ptr, write_left);
                    if (written <= 0)
                    {
                        rt_thread_mdelay(5);  /* cache full, wait */
                        continue;
                    }
                    write_ptr += written;
                    write_left -= written;
                }
                g_perf.total_pcm_bytes += bytes_needed;
            }
        }

        av_packet_unref(pkt);
    }

    /* Flush decoder */
    {
        AVPacket flush_pkt = {0};
        int got_frame = 0;
        do {
            got_frame = 0;
            avcodec_decode_audio4(dec_ctx, frame, &got_frame, &flush_pkt);
            if (got_frame)
            {
                int samples = frame->nb_samples;
                int channels = dec_ctx->channels;
                int bytes_needed = samples * channels * 2;
                if (bytes_needed <= PCM_FRAME_BUF_SIZE)
                {
                    if (dec_ctx->sample_fmt == AV_SAMPLE_FMT_FLTP)
                    {
                        for (int s = 0; s < samples; s++)
                            for (int c = 0; c < channels; c++)
                            {
                                float val = ((float *)frame->data[c])[s];
                                val = val < -1.0f ? -1.0f : (val > 1.0f ? 1.0f : val);
                                pcm_buf[s * channels + c] = (int16_t)(val * 32767);
                            }
                    }
                    else if (dec_ctx->sample_fmt == AV_SAMPLE_FMT_S16)
                    {
                        memcpy(pcm_buf, frame->data[0], bytes_needed);
                    }
                    {
                        uint8_t *wp = (uint8_t *)pcm_buf;
                        int wl = bytes_needed;
                        while (wl > 0)
                        {
                            int w = audio_write(g_audio_client, wp, wl);
                            if (w <= 0) { rt_thread_mdelay(5); continue; }
                            wp += w; wl -= w;
                        }
                        g_perf.total_pcm_bytes += bytes_needed;
                    }
                }
            }
        } while (got_frame);
    }

    g_perf.end_tick = dwt_get_cycles();

    /* Wait for audio hardware to finish playing remaining data
     * Calculate: PCM bytes / (sample_rate * channels * 2) = duration in seconds */
    {
        uint32_t bytes_per_sec = dec_ctx->sample_rate * dec_ctx->channels * 2;
        uint32_t remaining_ms = (g_perf.total_pcm_bytes * 1000) / bytes_per_sec;
        rt_kprintf("[FFMPEG] Waiting %d ms for playback to finish...\n", remaining_ms);
        rt_thread_mdelay(remaining_ms + 200);  /* +200ms margin */
    }

    /* Cleanup */
    rt_free(pcm_buf);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    audio_output_close();
    avcodec_close(dec_ctx);
    avformat_close_input(&fmt_ctx);

    perf_report();
    rt_kprintf("[FFMPEG] Done\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_play, play, Decode and play audio file);

static int cmd_stop(int argc, char *argv[])
{
    g_stop_flag = 1;
    rt_kprintf("[PLAY] stopping...\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_stop, stop, Stop playback);

/* =====================================================================
 *  Main
 * ===================================================================== */
int main(void)
{
    dwt_init();
    av_register_all();

    rt_kprintf("\n========================================\n");
    rt_kprintf("  FFmpeg Audio Decode Performance Test\n");
    rt_kprintf("  Streaming decode + play\n");
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
