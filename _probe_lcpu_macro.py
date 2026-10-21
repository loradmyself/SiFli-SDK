import json, subprocess, re, os

test_src = (
    '#include "bf0_hal.h"\n'
    '#include "lcpu_config_type.h"\n'
    'uint32_t probe = (uint32_t)LCPU_CONFIG_START_ADDR;\n'
)
src_path = os.path.join(os.getcwd(), '_probe_lcpu.c')
with open(src_path, 'w') as f:
    f.write(test_src)

def fix_path(m):
    return src_path.replace('\\', '\\\\')

for proj in [
    'example/bt/a2dp_sharing/project/build_dpi-hdk_lb57eybbn6_n16_dpi_hcpu',
    'example/Dual_Bluetooth_Headset_Walkie-Talkie/project/build_dpi-hdk_lb57gyd7n6_epd_hcpu',
]:
    db = json.load(open(proj + '/compile_commands.json', encoding='utf-8'))
    entry = None
    for e in db:
        if 'bf0_hal_lcpu_config' in e.get('file', ''):
            entry = e
            break
    cmd = entry.get('command') or ' '.join(entry.get('arguments'))
    gcc = os.path.join(
        r'C:\Users\YBL\.sifli\tools\arm-none-eabi-gcc\14.2.1\bin',
        'arm-none-eabi-gcc.exe',
    )
    cmd = cmd.replace('arm-none-eabi-gcc ', gcc + ' ', 1)
    cmd2 = re.sub(r'\S+bf0_hal_lcpu_config\.c', fix_path, cmd)
    cmd2 = re.sub(r'\s-o\s+\S+', ' -E -o - ', cmd2)
    cmd2 = re.sub(r'(?<=\s)-c(?=\s)', '', cmd2)
    r = subprocess.run(cmd2, shell=True, capture_output=True, text=True, errors='replace')
    out = r.stdout
    m = re.findall(r'uint32_t probe = \(uint32_t\)(.*?);', out)
    print('#####', proj.split('/')[1][:30])
    print('probe =', m[-1] if m else 'NOT FOUND')
    if not m:
        print('stderr:', r.stderr[-800:])
    incs = re.findall(r'^# \d+ "([^"]*mem_map\.h)"', out, re.M)
    print('mem_map.h:', sorted(set(incs))[:8])
    cincs = re.findall(r'^# \d+ "([^"]*custom_mem_map\.h)"', out, re.M)
    print('custom_mem_map.h:', sorted(set(cincs))[:8])

os.remove(src_path)
