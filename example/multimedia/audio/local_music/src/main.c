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
    #include <sys/stat.h>
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

    /* Open PCM file for saving */
    int pcm_fd = open("/decoded.pcm", O_WRONLY | O_CREAT | O_TRUNC);
    if (pcm_fd < 0)
        rt_kprintf("[FFMPEG] open /decoded.pcm failed, skip save\n");

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

            /* Save PCM to file */
            if (pcm_fd >= 0)
                write(pcm_fd, pcm_buf, bytes_needed);
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
                    if (pcm_fd >= 0)
                        write(pcm_fd, pcm_buf, bytes_needed);
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

    /* Close PCM file */
    if (pcm_fd >= 0)
    {
        close(pcm_fd);
        rt_kprintf("[FFMPEG] PCM saved to /decoded.pcm\n");
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

/* Forward declaration */
static int cmd_encode_real(int argc, char *argv[]);

/**
 * @brief encode thread entry (FFmpeg needs large stack, can't run in tshell)
 */
static void encode_thread_entry(void *param)
{
    char *args[3] = { "encode", NULL, NULL };
    int argc = 1;

    if (param)
    {
        static char path_buf[128];
        strncpy(path_buf, (const char *)param, sizeof(path_buf) - 1);
        /* Parse "pcm aac" from the param string */
        char *space = strchr(path_buf, ' ');
        if (space)
        {
            *space = '\0';
            args[1] = path_buf;
            args[2] = space + 1;
            argc = 3;
        }
        else
        {
            args[1] = path_buf;
            argc = 2;
        }
    }

    /* Call the actual encode logic */
    cmd_encode_real(argc, args);

    rt_free(param);
}

/**
 * @brief encode [pcm_file] [output_file]
 *   Encode PCM to AAC (default: iphone.pcm -> encoder.aac)
 */
static int cmd_encode_real(int argc, char *argv[])
{
    const char *pcm_path = (argc > 1) ? argv[1] : "/iphone.pcm";
    const char *aac_path = (argc > 2) ? argv[2] : "/encoder.aac";
    int ret;

    rt_kprintf("[ENCODE] %s -> %s\n", pcm_path, aac_path);

    /* PCM params (match the AAC decode output) */
    int sample_rate = 24000;
    int channels = 2;
    int bits_per_sample = 16;

    /* Open PCM file */
    int fd = open(pcm_path, O_RDONLY);
    if (fd < 0)
    {
        rt_kprintf("[ENCODE] open %s failed\n", pcm_path);
        return -1;
    }

    /* Get file size */
    int pcm_data_size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    rt_kprintf("[ENCODE] PCM file size: %d bytes\n", pcm_data_size);

    /* Read entire PCM file into memory */
    uint8_t *pcm_data = (uint8_t *)ffmpeg_alloc(pcm_data_size);
    if (!pcm_data)
    {
        rt_kprintf("[ENCODE] malloc %d bytes failed\n", pcm_data_size);
        close(fd);
        return -1;
    }
    int read_len = read(fd, pcm_data, pcm_data_size);
    close(fd);
    if (read_len != pcm_data_size)
    {
        rt_kprintf("[ENCODE] read incomplete: %d/%d\n", read_len, pcm_data_size);
        ffmpeg_free(pcm_data);
        return -1;
    }
    rt_kprintf("[ENCODE] PCM loaded: %d bytes\n", pcm_data_size);

    /* Find AAC encoder */
    AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
    {
        rt_kprintf("[ENCODE] AAC encoder not found\n");
        ffmpeg_free(pcm_data);
        return -1;
    }
    rt_kprintf("[ENCODE] encoder: %s\n", codec->name);

    /* Allocate encoder context */
    AVCodecContext *enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx)
    {
        rt_kprintf("[ENCODE] alloc encoder context failed\n");
        ffmpeg_free(pcm_data);
        return -1;
    }

    /* Set encoder params */
    enc_ctx->sample_rate = sample_rate;
    enc_ctx->channels = channels;
    enc_ctx->channel_layout = av_get_default_channel_layout(channels);
    enc_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;  /* AAC requires float planar */
    enc_ctx->bit_rate = 128000;  /* 128kbps */
    enc_ctx->time_base = (AVRational){1, sample_rate};

    /* Open encoder */
    ret = avcodec_open2(enc_ctx, codec, NULL);
    if (ret < 0)
    {
        char errbuf[64];
        av_strerror(ret, errbuf, sizeof(errbuf));
        rt_kprintf("[ENCODE] avcodec_open2 failed: %d (%s)\n", ret, errbuf);
        avcodec_free_context(&enc_ctx);
        ffmpeg_free(pcm_data);
        return -1;
    }

    /* Allocate output format context */
    AVFormatContext *fmt_ctx = NULL;
    ret = avformat_alloc_output_context2(&fmt_ctx, NULL, NULL, aac_path);
    if (ret < 0 || !fmt_ctx)
    {
        rt_kprintf("[ENCODE] alloc output context failed\n");
        avcodec_free_context(&enc_ctx);
        ffmpeg_free(pcm_data);
        return -1;
    }

    /* Add audio stream */
    AVStream *stream = avformat_new_stream(fmt_ctx, NULL);
    if (!stream)
    {
        rt_kprintf("[ENCODE] new stream failed\n");
        avformat_free_context(fmt_ctx);
        avcodec_free_context(&enc_ctx);
        ffmpeg_free(pcm_data);
        return -1;
    }
    stream->id = fmt_ctx->nb_streams - 1;
    avcodec_copy_context(stream->codec, enc_ctx);
    stream->time_base = enc_ctx->time_base;

    /* Open output file */
    ret = avio_open(&fmt_ctx->pb, aac_path, AVIO_FLAG_WRITE);
    if (ret < 0)
    {
        char errbuf[64];
        av_strerror(ret, errbuf, sizeof(errbuf));
        rt_kprintf("[ENCODE] avio_open failed: %d (%s)\n", ret, errbuf);
        avformat_free_context(fmt_ctx);
        avcodec_free_context(&enc_ctx);
        ffmpeg_free(pcm_data);
        return -1;
    }

    /* Write header */
    ret = avformat_write_header(fmt_ctx, NULL);
    if (ret < 0)
    {
        char errbuf[64];
        av_strerror(ret, errbuf, sizeof(errbuf));
        rt_kprintf("[ENCODE] write header failed: %d (%s)\n", ret, errbuf);
        avio_closep(&fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
        avcodec_free_context(&enc_ctx);
        ffmpeg_free(pcm_data);
        return -1;
    }

    /* Allocate frame and packet */
    AVFrame *frame = av_frame_alloc();
    AVPacket *pkt = av_packet_alloc();
    frame->nb_samples = enc_ctx->frame_size > 0 ? enc_ctx->frame_size : 1024;
    frame->format = enc_ctx->sample_fmt;
    frame->channel_layout = enc_ctx->channel_layout;
    av_frame_get_buffer(frame, 0);

    /* Encode loop */
    int frame_size_bytes = frame->nb_samples * channels * 2;  /* 16-bit PCM */
    int pcm_offset = 0;
    int pkt_count = 0;

    perf_reset();
    g_perf.start_tick = dwt_get_cycles();

    while (pcm_offset + frame_size_bytes <= pcm_data_size)
    {
        /* Make frame writable */
        av_frame_make_writable(frame);

        /* Convert int16 PCM -> float planar (required by AAC encoder) */
        int16_t *src = (int16_t *)(pcm_data + pcm_offset);
        for (int s = 0; s < frame->nb_samples; s++)
        {
            for (int c = 0; c < channels; c++)
            {
                float val = (float)src[s * channels + c] / 32768.0f;
                ((float *)frame->data[c])[s] = val;
            }
        }

        frame->pts = pcm_offset / frame_size_bytes;

        /* Encode with timing */
        uint32_t t0 = dwt_get_cycles();
        int got_output = 0;
        ret = avcodec_encode_audio2(enc_ctx, pkt, frame, &got_output);
        uint32_t t1 = dwt_get_cycles();

        if (ret < 0)
        {
            char errbuf[64];
            av_strerror(ret, errbuf, sizeof(errbuf));
            rt_kprintf("[ENCODE] encode error: %d (%s)\n", ret, errbuf);
            g_perf.error_count++;
            break;
        }

        uint32_t cycles = t1 - t0;
        g_perf.total_decode_cycles += cycles;  /* reuse stats */
        g_perf.frame_count++;
        if (cycles > g_perf.max_decode_cycles)
            g_perf.max_decode_cycles = cycles;
        if (g_perf.min_decode_cycles == 0 || cycles < g_perf.min_decode_cycles)
            g_perf.min_decode_cycles = cycles;

        if (got_output)
        {
            pkt->stream_index = stream->id;
            av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
            ret = av_interleaved_write_frame(fmt_ctx, pkt);
            if (ret < 0)
            {
                rt_kprintf("[ENCODE] write frame error: %d\n", ret);
            }
            else
            {
                pkt_count++;
            }
            av_packet_unref(pkt);
        }

        pcm_offset += frame_size_bytes;
    }

    /* Flush encoder */
    {
        int got_output = 0;
        do {
            got_output = 0;
            ret = avcodec_encode_audio2(enc_ctx, pkt, NULL, &got_output);
            if (ret < 0 || !got_output)
                break;

            pkt->stream_index = stream->id;
            av_packet_rescale_ts(pkt, enc_ctx->time_base, stream->time_base);
            av_interleaved_write_frame(fmt_ctx, pkt);
            av_packet_unref(pkt);
            pkt_count++;
        } while (got_output);
    }

    g_perf.end_tick = dwt_get_cycles();

    /* Write trailer */
    av_write_trailer(fmt_ctx);

    /* Get output file size */
    int aac_size = 0;
    {
        struct stat st;
        if (stat(aac_path, &st) == 0)
            aac_size = st.st_size;
    }

    perf_report();
    rt_kprintf("[ENCODE] Output: %s, %d packets, %d bytes\n", aac_path, pkt_count, aac_size);

    /* Cleanup */
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);
    avcodec_free_context(&enc_ctx);
    ffmpeg_free(pcm_data);

    rt_kprintf("[ENCODE] Done\n");
    return 0;
}

static int cmd_encode(int argc, char *argv[])
{
    /* Build arg string for thread */
    char *param = NULL;
    if (argc > 1)
    {
        int len = 0;
        for (int i = 1; i < argc; i++)
            len += strlen(argv[i]) + 1;
        param = rt_malloc(len);
        if (!param) return -1;
        param[0] = '\0';
        for (int i = 1; i < argc; i++)
        {
            if (i > 1) strcat(param, " ");
            strcat(param, argv[i]);
        }
    }

    rt_thread_t tid = rt_thread_create("encode", encode_thread_entry, param,
                                        32768, 20, 10);
    if (tid)
        rt_thread_startup(tid);
    else
        rt_kprintf("[ENCODE] create thread failed\n");

    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_encode, encode, Encode PCM to AAC file);

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

    /* Manually register AAC encoder (CONFIG_AAC_ENCODER macro not propagating) */
    {
        extern AVCodec ff_aac_encoder;
        avcodec_register(&ff_aac_encoder);
        rt_kprintf("[FFMPEG] Manually registered AAC encoder\n");
    }

    /* Manually register ADTS muxer (inside #if 0 in allformats.c) */
    {
        extern AVOutputFormat ff_adts_muxer;
        av_register_output_format(&ff_adts_muxer);
        rt_kprintf("[FFMPEG] Manually registered ADTS muxer\n");
    }

    rt_kprintf("\n========================================\n");
    rt_kprintf("  FFmpeg Audio Decode Performance Test\n");
    rt_kprintf("  Streaming decode + play\n");
    rt_kprintf("========================================\n");
    rt_kprintf("Commands:\n");
    rt_kprintf("  play [file]          - decode and play (default: /iphone.aac)\n");
    rt_kprintf("  encode [pcm] [aac]   - encode PCM to AAC\n");
    rt_kprintf("                        default: /iphone.pcm -> /encoder.aac\n");
    rt_kprintf("  stop                 - stop playback\n");
    rt_kprintf("========================================\n\n");

    while (1)
    {
        rt_thread_mdelay(1000);
    }
    return 0;
}
