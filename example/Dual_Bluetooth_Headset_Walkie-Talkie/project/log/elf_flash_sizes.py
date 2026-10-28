# -*- coding: utf-8 -*-
"""Per-symbol flash footprint via nm --print-size --size-sort.
Groups symbols by subsystem prefix to find reclaimable feature groups.
Only counts symbols in flash address range (0x1200xxxx..0x1210xxxx)."""
import subprocess, os, re, collections

HERE = os.path.dirname(os.path.abspath(__file__))
ELF = os.path.join(HERE, '..', 'build_spi-hdk_lb57bun3n6_hcpu', 'main.elf')
NM = r'C:\Users\YBL\.sifli\tools\arm-none-eabi-gcc\14.2.1\bin\arm-none-eabi-nm.exe'

out = subprocess.run([NM, '--print-size', '--size-sort', ELF],
                     capture_output=True).stdout.decode(errors='replace')

syms = []  # (size, addr, type, name)
for line in out.splitlines():
    parts = line.split()
    if len(parts) == 4:
        try:
            addr = int(parts[0], 16); size = int(parts[1], 16)
        except ValueError:
            continue
        syms.append((size, addr, parts[2], parts[3]))

flash = [(s, a, t, n) for (s, a, t, n) in syms if 0x12000000 <= a < 0x12100000]
total = sum(s for s, a, t, n in flash)
print('flash symbol total:', total, ' symbols:', len(flash))

def grp(name):
    n = name.lower()
    for pat, tag in [
        (r'^(ff_|f_|fread|fwrite|fopen|fclose|fls_|chk_chr|get_ldnumber|follow_path|dir_|create_name|dir_next|get_fileinfo|put_ldnumber|mem_set|mem_cpy|mem_cmp|chr_cmp|follow_path)', 'fatfs'),
        (r'^dfs_', 'dfs'),
        (r'^(fdb_|fal_)', 'flashdb/fal'),
        (r'^(ulog_|do_output|output_lock|output_unlock|get_log_buf|ulog_async)', 'ulog'),
        (r'^(rt_rbb|rbb)', 'rbb'),
        (r'^(cmd_|msh_|finsh_|shell_|list_|_cmd)', 'shell'),
        (r'(mp3|id3|mad_|huffman|layer_)', 'mp3'),
        (r'^(cmsis_|osRtx|svcRtx)', 'cmsis-rtos2'),
        (r'^(bf0_hal_pcd|usb_|usbd_|dcd_)', 'usb'),
        (r'^(HAL_SD|sd_ex|stm32_sd)', 'sd'),
    ]:
        if re.match(pat, n):
            return tag
    return 'other'

g = collections.Counter()
for s, a, t, n in flash:
    g[grp(n)] += s
print()
for tag, sz in g.most_common():
    print(f'{sz:8d}  {tag}')

print('\nbiggest symbols:')
for s, a, t, n in sorted(flash, reverse=True)[:25]:
    print(f'{s:7d}  {t} {n}')
