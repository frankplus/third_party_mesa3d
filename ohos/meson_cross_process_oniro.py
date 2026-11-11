#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (c) 2026 Eclipse Oniro for OpenHarmony contributors.
# SPDX-License-Identifier: MIT
#
# Generate the meson cross file and the pkg-config set for the Oniro emulator
# Mesa build (llvmpipe/softpipe/virgl on OpenHarmony), for either architecture.
#
# meson_cross_process86.py does the same for x86_64 with diemit's prebuilt LLVM
# list; this generator takes the target architecture and an LLVM library
# directory (a static LLVM built for <arch>-linux-ohos, e.g. by the Oniro
# device/soc build_llvm.sh) and links every libLLVM*.a in it, so the list
# never goes stale. meson already wraps the link arguments in one
# --start-group/--end-group (lld rejects a nested one), which resolves the
# order between the archives.
#
# Usage: meson_cross_process_oniro.py <ohos root> <product> <arch> <llvm lib dir>
#   arch: arm64 | x86_64
# Writes thirdparty/mesa3d/cross_file and thirdparty/mesa3d/pkgconfig/*.pc
# relative to the current directory (the product's out/ directory).

import os
import re
import sys

ARCHES = {
    'arm64': ('aarch64-linux-ohosmusl', 'aarch64-linux-ohos', 'aarch64', 'armv8'),
    'x86_64': ('x86_64-linux-ohosmusl', 'x86_64-linux-ohos', 'x86_64', 'x86_64'),
}

CROSS_FILE = '''
[built-in options]

c_args = [
    '--target=@TARGET@',
    '--sysroot=@SYSROOT@',
    '-fno-emulated-tls',
    '-fPIC',
    '-g',
    '-O2',
    '-D_FORTIFY_SOURCE=2',
    '-fstack-protector-all',
    ]

cpp_args = [
    '--target=@TARGET@',
    '--sysroot=@SYSROOT@',
    '-fno-emulated-tls',
    '-fPIC',
    '-g',
    '-O2',
    '-D_FORTIFY_SOURCE=2',
    '-fstack-protector-all',
    ]

c_link_args = [
    '--target=@TARGET@',
    '-fPIC',
    '--sysroot=@SYSROOT@',
    '-L@SYSROOT@/usr/lib/@LIBTRIPLE@',
    '-L@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/lib/clang/current/lib/@LIBTRIPLE@',
    '-L@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/lib/@LIBTRIPLE@/c++',
    '--rtlib=compiler-rt',
    ]

cpp_link_args = [
    '--target=@TARGET@',
    '--sysroot=@SYSROOT@',
    '-L@SYSROOT@/usr/lib/@LIBTRIPLE@',
    '-L@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/lib/clang/current/lib/@LIBTRIPLE@',
    '-L@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/lib/@LIBTRIPLE@/c++',
    '-fPIC',
    '-Wl,--exclude-libs=libunwind_llvm.a',
    '-Wl,--exclude-libs=libc++_static.a',
    '-Wl,--warn-shared-textrel',
    '--rtlib=compiler-rt',
    '-L@LLVMLIB@',
@LLVMLIBS@
    ]

[binaries]
ar = '@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-ar'
c = ['ccache', '@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang']
cpp = ['ccache', '@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/bin/clang++']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '@ROOT@/prebuilts/clang/ohos/linux-x86_64/llvm/bin/llvm-strip'
llvm-config = '@LLVMCONFIG@'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = '@CPUFAMILY@'
cpu = '@CPU@'
endian = 'little'
'''


def generate_cross_file(root, product, arch, llvm_lib_dir, llvm_config):
    target, libtriple, cpu_family, cpu = ARCHES[arch]
    sysroot = os.path.join(root, 'out', product, 'obj', 'third_party', 'musl')
    libs = sorted(f for f in os.listdir(llvm_lib_dir)
                  if f.startswith('libLLVM') and f.endswith('.a'))
    if not libs:
        sys.exit('no libLLVM*.a in ' + llvm_lib_dir)
    llvm_libs = '\n'.join("    '-l%s'," % f[3:-2] for f in libs)
    content = CROSS_FILE
    for key, val in (('@TARGET@', target), ('@LIBTRIPLE@', libtriple),
                     ('@SYSROOT@', sysroot), ('@ROOT@', root),
                     ('@LLVMLIB@', llvm_lib_dir), ('@LLVMLIBS@', llvm_libs),
                     ('@LLVMCONFIG@', llvm_config),
                     ('@CPUFAMILY@', cpu_family), ('@CPU@', cpu)):
        content = content.replace(key, val)
    os.makedirs('thirdparty/mesa3d', exist_ok=True)
    with open('thirdparty/mesa3d/cross_file', 'w') as f:
        f.write(content)


# Prebuilt archives in this directory that exist for x86_64 only. Both are
# optional for the emulator build (SPIR-V validation in vtn; CLC is off), so
# other architectures go without them.
X86_64_ONLY_PC = ('SPIRV-Tools.pc', 'LLVMSPIRVLib.pc')


def generate_pc_files(root, product, arch):
    template_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                'pkgconfig_template')
    os.makedirs('thirdparty/mesa3d/pkgconfig', exist_ok=True)
    for name in os.listdir(template_dir):
        path = os.path.join(template_dir, name)
        if os.path.isdir(path):
            continue
        if arch != 'x86_64' and name in X86_64_ONLY_PC:
            stale = os.path.join('thirdparty/mesa3d/pkgconfig', name)
            if os.path.exists(stale):
                os.remove(stale)
            continue
        with open(path) as f:
            content = f.read()
        content = content.replace('ohos_project_directory_stub', root)
        content = content.replace('ohos-arm-release', product)
        if name == 'expat.pc':
            content = content.replace('skia_folder_stub', 'skia/m133')
            content = content.replace('expat_lib_stub', 'expatm133')
        if name == 'libqos.pc':
            # The template points at the arm64 SDK sysroot; link the product's
            # own libqos instead.
            content = re.sub(r'^libdir=.*$',
                             'libdir=${ohos_project_dir}/out/' + product +
                             '/resourceschedule/qos_manager',
                             content, flags=re.M)
        with open(os.path.join('thirdparty/mesa3d/pkgconfig', name), 'w') as f:
            f.write(content)


def main():
    if len(sys.argv) != 6:
        sys.exit('usage: meson_cross_process_oniro.py <ohos root> <product> '
                 '<arm64|x86_64> <llvm lib dir> <llvm-config>')
    root, product, arch, llvm_lib_dir, llvm_config = sys.argv[1:]
    if arch not in ARCHES:
        sys.exit('unknown arch ' + arch)
    generate_cross_file(root, product.lower(), arch, llvm_lib_dir, llvm_config)
    generate_pc_files(root, product.lower(), arch)


if __name__ == '__main__':
    main()
