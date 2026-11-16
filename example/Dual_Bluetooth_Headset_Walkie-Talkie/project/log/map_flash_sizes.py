# -*- coding: utf-8 -*-
"""Aggregate flash-resident section sizes (.text/.rodata/.data/.ARM.*) per
object from main.map. Input sections at real link addresses (0x12xxxxxx),
not the 0-address discard listing at the top of the map."""
import re, os, sys

MAP = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   '..', 'build_spi-hdk_lb57bun3n6_hcpu', 'main.map')
objs = {}
pat = re.compile(r'^\s+(\.(?:text|rodata|data|ARM\.exidx|ARM\.extab)[.\w]*)'
                 r'\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)')
for line in open(MAP, errors='replace'):
    m = pat.match(line)
    if m:
        addr, size, obj = int(m.group(2), 16), int(m.group(3), 16), m.group(4)
        # skip the discard listing at map top (address 0)
        if addr < 0x12000000:
            continue
        if size > 0 and obj.endswith(('.o', '.obj')) and 'arm-none-eabi' not in obj:
            name = obj.replace('\\', '/').split('/')[-1]
            objs[name] = objs.get(name, 0) + size

total = sum(objs.values())
print('flash-resident total:', total)
for o, s in sorted(objs.items(), key=lambda kv: -kv[1])[:40]:
    print(f'{s:8d}  {o}')

# group by subsystem keywords
groups = {
    'dfs/fatfs': ['ff.o', 'ffunicode.o', 'dfs', 'select.o', 'poll.o'],
    'flashdb/fal': ['fdb', 'fal'],
    'ulog': ['ulog', 'ulog_console'],
    'rbb': ['ringblk', 'rbb'],
    'shell': ['msh', 'finsh', 'cmd.o', 'shell', 'tshell'],
    'cmsis-rtos2': ['cmsis'],
    'mp3/localmusic': ['mp3', 'id3'],
}
print()
for g, keys in groups.items():
    n = sum(v for k, v in objs.items() if any(x in k for x in keys))
    print(f'{g:16s} {n:8d}')
