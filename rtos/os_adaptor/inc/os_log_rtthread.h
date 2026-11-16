/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __LOG__RT_H
#define __LOG__RT_H

#undef LOG_I
#undef LOG_W
#undef LOG_D
#undef LOG_E
#undef LOG_HEX
#undef DBG_ERROR
#undef DBG_WARNING
#include "rtdbg.h"

/* rtdbg.h stubs LOG_BIN/LOG_BIN_MIX to nothing. The Bluetooth HCI-on-log
 * path (bluetooth_config.c tl_log_write) relies on them to dump the raw
 * HCI byte stream for the siflitrace tool — without this,
 * CONFIG_HCI_ON_LOG compiles fine but outputs zero bytes. Restore the
 * ulog_binary() implementation here (raw bytes, parsed by siflitrace;
 * a normal terminal will not render them as text — that is expected). */
#undef LOG_BIN
#undef LOG_BIN_MIX
#ifdef RT_USING_ULOG
#include "ulog_def.h"
void ulog_binary(rt_uint8_t *buf, rt_size_t size, rt_uint8_t is_mix);
#define LOG_BIN(buf, size)              ulog_bin(buf, size, 0)
#define LOG_BIN_MIX(buf, size)          ulog_bin(buf, size, 1)
#endif

#endif //__LOG__RT_H

