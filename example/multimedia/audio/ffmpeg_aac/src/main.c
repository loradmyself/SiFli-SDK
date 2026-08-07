/*
 * SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FFmpeg AAC encode / decode example
 *   - aac_test [seconds]  -- record mic -> encode AAC -> decode & play
 *   - aac_enc             -- encode /mic_record.pcm -> /test.aac
 *   - aac_play            -- decode /test.aac & play to speaker
 */

#include <rtthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

/* FFmpeg headers */
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"
#include "libavutil/channel_layout.h"

/* SiFli audio server */
#include "audio_server.h"
#include "os_adaptor.h"

/* FFmpeg media decoder (for playback) */
#include "media_dec.h"

#if RT_USING_DFS
    #include "dfs_file.h"
    #include "dfs_posix.h"
#endif
#include "log.h"
#include "mem_section.h"

#define DBG_TAG           "aac"
#define DBG_LVL           LOG_LVL_INFO

/* File paths */
#define MIC_PCM_FILE            "/mic_record.pcm"
#define AAC_FILE                "/test.aac"

/* Audio parameters */
#define DEFAULT_SAMPLE_RATE     16000
#define DEFAULT_CHANNELS        1
#define DEFAULT_BPS             16
#define AAC_FRAME_SAMPLES       1024    /* AAC frame size is always 1024 samples */
#define MIC_DEFAULT_SECONDS     10
#define MIC_PCM_BUF_SIZE        (MIC_DEFAULT_SECONDS * DEFAULT_SAMPLE_RATE * 2)
#define AAC_STACK_SIZE          (20 * 1024)

/* Thread handles */
static struct rt_thread aac_thread;
static uint32_t g_record_seconds;

/* Recording state */
static uint8_t drop_noise_cnt;
static uint8_t *g_pcm_buf;
static uint32_t g_pcm_len, g_pcm_cap;

/* Playback thread */
static struct rt_thread aac_play_thread;
static uint32_t aac_play_stack[AAC_STACK_SIZE / sizeof(uint32_t)];
static char g_aac_play_path[128];

/* FFmpeg decode handle */
static ffmpeg_handle g_ffmpeg_handle = NULL;

/* L2 retained buffer for PCM recording */
L2_RET_BSS_SECT_BEGIN(aac_pcm)
ALIGN(4) static uint8_t g_pcm_storage[MIC_PCM_BUF_SIZE] L2_RET_BSS_SECT(aac_pcm);
ALIGN(4) static uint32_t aac_stack[AAC_STACK_SIZE / sizeof(uint32_t)] L2_RET_BSS_SECT(aac_pcm);
L2_RET_BSS_SECT_END

/* =====================================================================
 *  Filesystem init
 * ===================================================================== */

#ifndef FS_REGION_START_ADDR
    #error "Need to define file system start address!"
#endif

#define FS_ROOT "root"

static int mnt_init(void)
{
    register_mtd_device(FS_REGION_START_ADDR, FS_REGION_SIZE, FS_ROOT);
    if (dfs_mount(FS_ROOT, "/", "elm", 0, 0) == 0)
    {
        rt_kprintf("mount fs on flash to root success\n");
    }
    else
    {
        rt_kprintf("mount fs on flash to root fail\n");
        if (dfs_mkfs("elm", FS_ROOT) == 0)
        {
            rt_kprintf("make elm fs on flash success, mount again\n");
            if (dfs_mount(FS_ROOT, "/", "elm", 0, 0) == 0)
                rt_kprintf("mount fs on flash success\n");
            else
                rt_kprintf("mount to fs on flash fail\n");
        }
        else
            rt_kprintf("dfs_mkfs elm flash fail\n");
    }
    return RT_EOK;
}
INIT_ENV_EXPORT(mnt_init);

/* =====================================================================
 *  Microphone recording
 * ===================================================================== */

static int record_callback(audio_server_callback_cmt_t cmd, void *callback_userdata, uint32_t reserved)
{
    (void)callback_userdata;
    if (cmd == as_callback_cmd_data_coming)
    {
        audio_server_coming_data_t *p = (audio_server_coming_data_t *)reserved;
        if (drop_noise_cnt < 20)
        {
            drop_noise_cnt++;
            return 0;
        }
        if (g_pcm_len + p->data_len <= g_pcm_cap)
        {
            memcpy(g_pcm_buf + g_pcm_len, p->data, p->data_len);
            g_pcm_len += p->data_len;
        }
    }
    return 0;
}

static int mic_record_to_file(uint32_t seconds)
{
    int fd;
    audio_parameter_t pa = {0};

    pa.write_bits_per_sample = 16;
    pa.write_channnel_num   = 1;
    pa.write_samplerate     = 16000;
    pa.read_bits_per_sample = 16;
    pa.read_channnel_num    = 1;
    pa.read_samplerate      = 16000;
    pa.read_cache_size      = 0;
    pa.write_cache_size     = 2048;
    drop_noise_cnt = 0;
    g_pcm_cap = seconds * 16000 * 2;
    g_pcm_len = 0;

    if (g_pcm_cap > MIC_PCM_BUF_SIZE)
    {
        rt_kprintf("aac: record %u s exceeds buffer\n", seconds);
        return -1;
    }

    g_pcm_buf = g_pcm_storage;
    audio_client_t client = audio_open(AUDIO_TYPE_LOCAL_RECORD, AUDIO_RX, &pa,
                                       record_callback, NULL);
    if (!client)
    {
        rt_kprintf("aac: audio_open record failed\n");
        return -1;
    }

    rt_kprintf("aac: recording %u seconds ...\n", seconds);
    for (uint32_t i = 0; i < seconds; i++)
    {
        rt_thread_mdelay(1000);
        rt_kprintf("  %u/%u s\n", i + 1, seconds);
    }

    audio_close(client);

    fd = open(MIC_PCM_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY);
    if (fd < 0)
    {
        rt_kprintf("aac: open %s failed\n", MIC_PCM_FILE);
        return -1;
    }
    write(fd, g_pcm_buf, g_pcm_len);
    close(fd);
    g_pcm_buf = NULL;
    rt_kprintf("aac: record done -> %s (%u bytes)\n", MIC_PCM_FILE, g_pcm_len);
    return 0;
}

/* =====================================================================
 *  AAC Encoding (FFmpeg)
 * ===================================================================== */

/**
 * @brief Write ADTS header for one AAC frame
 *
 * ADTS header is 7 bytes (no CRC) or 9 bytes (with CRC).
 * We write 7-byte headers for simplicity.
 */
static void write_adts_header(uint8_t *buf, int total_size, int sample_rate_idx, int channels)
{
    int profile = 2;    /* AAC-LC */
    int freq_idx = sample_rate_idx;
    int chan_cfg = channels;

    int full_frame_size = total_size + 7;  /* ADTS header + raw AAC data */

    buf[0] = 0xFF;
    buf[1] = 0xF1;                             /* MPEG-4, Layer 0, no CRC */
    buf[2] = ((profile - 1) << 6) | (freq_idx << 2) | (chan_cfg >> 2);
    buf[3] = ((chan_cfg & 0x3) << 6) | ((full_frame_size >> 11) & 0x03);
    buf[4] = (full_frame_size >> 3) & 0xFF;
    buf[5] = ((full_frame_size & 0x07) << 5) | 0x1F;
    buf[6] = 0xFC;
}

/**
 * @brief Get FFmpeg sample rate index for ADTS header
 */
static int get_sample_rate_idx(int sample_rate)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000,
                                  24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    for (int i = 0; i < 13; i++)
    {
        if (rates[i] == sample_rate)
            return i;
    }
    return 4;   /* default to 44100 Hz */
}

/**
 * @brief Encode raw PCM file to AAC using FFmpeg encoder
 *
 * Flow: read int16 PCM -> convert to float planar -> avcodec_send_frame
 *        -> avcodec_receive_packet -> prepend ADTS header -> write .aac file
 */
static int aac_encode_file(const char *pcm_path, const char *aac_path)
{
    const AVCodec *codec = NULL;
    AVCodecContext *enc_ctx = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    int pcm_fd = -1;
    int out_fd = -1;
    int ret = -1;
    uint32_t total_samples = 0;

    /* 1. Find AAC encoder */
    codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!codec)
    {
        rt_kprintf("aac_enc: AAC encoder not found\n");
        goto _enc_exit;
    }

    /* 2. Allocate encoder context */
    enc_ctx = avcodec_alloc_context3(codec);
    if (!enc_ctx)
    {
        rt_kprintf("aac_enc: alloc context failed\n");
        goto _enc_exit;
    }

    enc_ctx->codec_id     = AV_CODEC_ID_AAC;
    enc_ctx->codec_type   = AVMEDIA_TYPE_AUDIO;
    enc_ctx->sample_fmt   = AV_SAMPLE_FMT_FLTP;  /* AAC encoder requires float planar */
    enc_ctx->sample_rate  = DEFAULT_SAMPLE_RATE;
    enc_ctx->channel_layout = AV_CH_LAYOUT_MONO;
    enc_ctx->channels     = DEFAULT_CHANNELS;
    enc_ctx->bit_rate     = 64000;   /* 64 kbps for 16kHz mono */

    /* 3. Open encoder */
    ret = avcodec_open2(enc_ctx, codec, NULL);
    if (ret < 0)
    {
        rt_kprintf("aac_enc: open encoder failed, err=%d\n", ret);
        goto _enc_exit;
    }

    /* 4. Allocate frame and packet */
    frame = av_frame_alloc();
    if (!frame)
    {
        rt_kprintf("aac_enc: alloc frame failed\n");
        goto _enc_exit;
    }
    frame->nb_samples   = AAC_FRAME_SAMPLES;
    frame->format       = enc_ctx->sample_fmt;
    frame->channel_layout = enc_ctx->channel_layout;

    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
    {
        rt_kprintf("aac_enc: get frame buffer failed, err=%d\n", ret);
        goto _enc_exit;
    }

    pkt = av_packet_alloc();
    if (!pkt)
    {
        rt_kprintf("aac_enc: alloc packet failed\n");
        goto _enc_exit;
    }

    /* 5. Open input PCM file */
    pcm_fd = open(pcm_path, O_RDONLY | O_BINARY);
    if (pcm_fd < 0)
    {
        rt_kprintf("aac_enc: open %s failed\n", pcm_path);
        goto _enc_exit;
    }

    /* 6. Open output AAC file */
    out_fd = open(aac_path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY);
    if (out_fd < 0)
    {
        rt_kprintf("aac_enc: open %s failed\n", aac_path);
        goto _enc_exit;
    }

    rt_kprintf("aac_enc: encoding %s -> %s (%dHz %dch %dbps)\n",
               pcm_path, aac_path, DEFAULT_SAMPLE_RATE, DEFAULT_CHANNELS, DEFAULT_BPS);

    /* 7. Encode loop: read PCM -> convert to float -> encode -> write ADTS */
    const int bytes_per_frame = AAC_FRAME_SAMPLES * DEFAULT_CHANNELS * (DEFAULT_BPS / 8);
    uint8_t *raw_buf = (uint8_t *)rt_malloc(bytes_per_frame);
    if (!raw_buf)
    {
        rt_kprintf("aac_enc: malloc raw_buf failed\n");
        goto _enc_exit;
    }

    while (1)
    {
        int rd = read(pcm_fd, raw_buf, bytes_per_frame);
        if (rd <= 0)
            break;

        int samples_read = rd / (DEFAULT_CHANNELS * (DEFAULT_BPS / 8));

        /* Ensure frame is writable before filling */
        av_frame_make_writable(frame);

        /* Convert int16 interleaved PCM to float planar (AV_SAMPLE_FMT_FLTP) */
        int16_t *pcm16 = (int16_t *)raw_buf;
        float *flt = (float *)frame->data[0];
        for (int s = 0; s < samples_read; s++)
        {
            flt[s] = (float)pcm16[s] / 32768.0f;
        }

        frame->nb_samples = samples_read;

        /* Encode */
        ret = avcodec_send_frame(enc_ctx, frame);
        if (ret < 0)
        {
            rt_kprintf("aac_enc: send_frame error, err=%d\n", ret);
            break;
        }

        /* Receive all available packets */
        while (1)
        {
            ret = avcodec_receive_packet(enc_ctx, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
            {
                rt_kprintf("aac_enc: receive_packet error, err=%d\n", ret);
                goto _enc_loop_end;
            }

            /* Write ADTS header + AAC frame data */
            uint8_t adts[7];
            int freq_idx = get_sample_rate_idx(DEFAULT_SAMPLE_RATE);
            write_adts_header(adts, pkt->size, freq_idx, DEFAULT_CHANNELS);

            write(out_fd, adts, 7);
            write(out_fd, pkt->data, pkt->size);

            av_packet_unref(pkt);
        }

        total_samples += samples_read;
    }

    /* Flush encoder */
    avcodec_send_frame(enc_ctx, NULL);
    while (1)
    {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0)
            break;

        uint8_t adts[7];
        int freq_idx = get_sample_rate_idx(DEFAULT_SAMPLE_RATE);
        write_adts_header(adts, pkt->size, freq_idx, DEFAULT_CHANNELS);

        write(out_fd, adts, 7);
        write(out_fd, pkt->data, pkt->size);

        av_packet_unref(pkt);
    }

    rt_kprintf("aac_enc: done, %u samples encoded\n", total_samples);
    ret = 0;

_enc_loop_end:
    rt_free(raw_buf);

_enc_exit:
    if (pkt)     av_packet_free(&pkt);
    if (frame)   av_frame_free(&frame);
    if (enc_ctx) avcodec_free_context(&enc_ctx);
    if (pcm_fd >= 0)  close(pcm_fd);
    if (out_fd >= 0)  close(out_fd);
    return ret;
}

/* =====================================================================
 *  AAC Decoding & Playback (via FFmpeg media player)
 * ===================================================================== */

static int ffmpeg_notify_callback(uint32_t user_data, ffmpeg_cmd_e cmd, uint32_t val)
{
    (void)user_data;
    (void)val;

    switch (cmd)
    {
    case e_ffmpeg_play_to_end:
        rt_kprintf("aac_play: play to end\n");
        break;
    case e_ffmpeg_play_to_error:
        rt_kprintf("aac_play: play error\n");
        break;
    default:
        break;
    }
    return 0;
}

static void *ffmpeg_mem_malloc(size_t size)
{
    return rt_malloc((rt_size_t)size);
}

static void ffmpeg_mem_free(void *ptr)
{
    rt_free(ptr);
}

static int aac_decode_and_play(const char *aac_path)
{
    rt_kprintf("aac_play: decoding & playing %s\n", aac_path);

    audio_server_set_private_volume(AUDIO_TYPE_LOCAL_MUSIC, 15);

    ffmpeg_config_t cfg = {0};
    cfg.src = e_src_localfile;
    cfg.audio_enable = 1;
    cfg.video_enable = 0;   /* audio only */
    cfg.is_loop = 0;
    cfg.file_path = aac_path;
    cfg.notify = ffmpeg_notify_callback;
    cfg.mem_malloc = ffmpeg_mem_malloc;
    cfg.mem_free = ffmpeg_mem_free;

    int ret = ffmpeg_open(&g_ffmpeg_handle, &cfg, 0);
    if (ret == 0)
    {
        rt_kprintf("aac_play: open success, playing...\n");
    }
    else
    {
        rt_kprintf("aac_play: open failed: %d\n", ret);
    }

    return ret;
}

static void aac_play_stop(void)
{
    if (g_ffmpeg_handle)
    {
        rt_kprintf("aac_play: stopping\n");
        ffmpeg_close(g_ffmpeg_handle);
        g_ffmpeg_handle = NULL;
    }
}

/* =====================================================================
 *  Shell commands
 * ===================================================================== */

static void aac_play_thread_entry(void *param)
{
    (void)param;
    aac_decode_and_play(g_aac_play_path);
    rt_kprintf("aac_play: thread exit\n");
}

static void aac_test_thread(void *param)
{
    uint32_t seconds = g_record_seconds;
    (void)param;

    /* Step 1: Record mic to PCM file */
    if (mic_record_to_file(seconds) != 0)
        return;

    /* Step 2: Encode PCM to AAC */
    if (aac_encode_file(MIC_PCM_FILE, AAC_FILE) != 0)
        return;

    /* Step 3: Decode AAC and play */
    aac_decode_and_play(AAC_FILE);

    rt_kprintf("aac_test: all done!\n");
}

/**
 * @brief aac_test [seconds]
 *   Record mic -> encode AAC -> decode & play (default 10s)
 */
static int cmd_aac_test(int argc, char *argv[])
{
    g_record_seconds = (argc > 1) ? (uint32_t)atoi(argv[1]) : MIC_DEFAULT_SECONDS;
    rt_thread_init(&aac_thread, "aac_test", aac_test_thread, NULL,
                   aac_stack, AAC_STACK_SIZE, RT_THREAD_PRIORITY_HIGH, 10);
    rt_thread_startup(&aac_thread);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_aac_test, aac_test, Record mic then AAC encode and play);

/**
 * @brief aac_enc [in] [out]
 *   Encode raw PCM to AAC (default: /mic_record.pcm -> /test.aac)
 */
static int cmd_aac_enc(int argc, char *argv[])
{
    const char *in  = (argc > 1) ? argv[1] : MIC_PCM_FILE;
    const char *out = (argc > 2) ? argv[2] : AAC_FILE;
    return aac_encode_file(in, out);
}
MSH_CMD_EXPORT_ALIAS(cmd_aac_enc, aac_enc, Encode PCM to AAC);

/**
 * @brief aac_play [file]
 *   Decode AAC and play to speaker (default: /test.aac)
 */
static int cmd_aac_play(int argc, char *argv[])
{
    const char *path = (argc > 1) ? argv[1] : AAC_FILE;

    rt_strncpy(g_aac_play_path, path, sizeof(g_aac_play_path) - 1);
    g_aac_play_path[sizeof(g_aac_play_path) - 1] = '\0';

    rt_thread_init(&aac_play_thread, "aac_play", aac_play_thread_entry, NULL,
                   aac_play_stack, AAC_STACK_SIZE, RT_THREAD_PRIORITY_HIGH, 10);
    rt_thread_startup(&aac_play_thread);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_aac_play, aac_play, Decode AAC and play);

/**
 * @brief aac_stop
 *   Stop current AAC playback
 */
static int cmd_aac_stop(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    aac_play_stop();
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_aac_stop, aac_stop, Stop AAC playback);

/* =====================================================================
 *  Main entry
 * ===================================================================== */

int main(void)
{
    rt_kprintf("\n[FFMPEG_AAC] AAC Encode/Decode Example.\n");
    rt_kprintf("  Commands:\n");
    rt_kprintf("    aac_test [seconds]  - record -> encode -> play\n");
    rt_kprintf("    aac_enc [in] [out]  - encode PCM to AAC\n");
    rt_kprintf("    aac_play [file]     - decode AAC & play\n");
    rt_kprintf("    aac_stop            - stop playback\n");

    while (1)
    {
        rt_thread_mdelay(10000);
    }
    return 0;
}
