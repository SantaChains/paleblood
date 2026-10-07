#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan and sdl3, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# Windows: MSYS2 CLANG64 shell (README "Windows": clang, libc++ and lld, whose threads and locks
# are the Win32 ones). The GPU library is static there (a DLL cannot leave the runtime_* symbols
# to the executable), the runtime adds its Win32 files (src/win32_*.c), runs without pthreads,
# and the executable keeps a console and bottom-up addresses (guest-visible host memory must
# stay below 1 TiB).
windows=
case $(uname -s) in MINGW*|MSYS*|CYGWIN*) windows=1 ;; esac
cstd=(-std=c11)
threads=(-pthread)
if [[ -n $windows ]]; then
    threads=()
    libraries=("${libraries[@]/-mwindows/-mconsole}")
    cstd=(-std=gnu11 -D_FILE_OFFSET_BITS=64 -D_WIN32_WINNT=0x0A00)
fi
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
# gpu/third_party/{fsr-vulkan,imgui} and third_party/LibAtrac9 are committed as ordinary files,
# not submodules (.gitmodules records their origins for provenance only; no 160000 gitlink
# exists), so `git clone --recursive` is not required and the guard below never fires on a
# complete clone. It stays for a working tree where those directories are missing.
#
# The FSR-Vulkan delta (gpu/patches/fsr-vulkan) is applied to the working tree once. On a fresh
# clone the committed tree already carries it, so the reverse check succeeds and this is a no-op.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
# Reconfigure only when the cached settings differ: a cmake configure costs seconds per
# launch, while ninja already re-runs cmake itself (replaying the original flags) when
# CMakeLists.txt or the toolchain changes.
if [[ -f out/gpu/build.ninja && $(cat out/gpu/.bbport-config 2>/dev/null) == "$pgo ${BB_LTO:-ON}" ]]; then
    echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON} (cached configure)"
else
    cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
        -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" >/dev/null
    printf '%s\n' "$pgo ${BB_LTO:-ON}" > out/gpu/.bbport-config
    echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
fi
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
# -rdynamic is a GCC/lld spelling that clang rejects outright ("argument unused"), and the
# Windows path below replaces this array anyway; the portable spelling is --export-dynamic,
# which lld accepts on both toolchains.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -Wl,--export-dynamic)
runtime=(src/runtime*.c)
link=(-no-pie)
res=()
if [[ -n $windows ]]; then
    # out/gpu/bbgpu_link.txt (gpu/CMakeLists.txt): libbbgpu.a and its dependencies, one per line.
    mapfile -t gpu < out/gpu/bbgpu_link.txt
    runtime+=(src/win32_*.c)
    # The static library's link-time optimization (ThinLTO in lld) runs here; on Linux it runs in
    # the .so link.
    link=(-Wl,--disable-dynamicbase,--disable-high-entropy-va -lwinmm -lws2_32 -lpsapi -flto=thin)
    # Explorer/taskbar icon: the resource compiles to a plain COFF object.
    windres -O coff -I logo src/probe.rc -o out/probe.res
    res=(out/probe.res)
fi
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
# Windows: the link includes the GPU library's link-time optimization (a minute or more), and
# run.bat builds before every start, so an up-to-date executable is kept.
if [[ -n $windows && -f out/bb-probe.exe && -f out/bb-gpu-capabilities.exe &&
      -z $(find src gpu/bbgpu.h src/probe.rc out/gpu/libbbgpu.a out/gpu/bbgpu_link.txt out/libatrac9.a build.sh \
               tools/gpu_capabilities.c logo/bloodborne.ico -newer out/bb-probe.exe -print -quit) ]]; then
    echo "Up to date: $PWD/out/bb-probe.exe"
else
"$CC" "${cstd[@]}" -O2 -g -Wall -Wextra -Werror "${threads[@]}" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c "${res[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
"$CC" "${cstd[@]}" -O2 -Wall -Wextra -Werror tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
fi
if [[ ${1:-} == --test ]]; then
    # runtime_file.c reaches the host through these on Windows (posix_pread/pwrite/rename and
    # inet_pton/ntop); without them the link fails on undefined Win32 shims.
    win_compat=()
    [[ -n $windows ]] && win_compat=(src/win32_compat.c)
    # The main binary links ${link[@]} (on Windows: ASLR off, so the fixed guest address
    # space at 0x800000000 is still free when runtime_memory reserves it). Tests that link
    # runtime_memory.c need the same treatment or the reservation fails with error 487.
    # test_pad.c is POSIX-shaped (mkdtemp/setenv/usleep); it only builds where those exist.
    if [[ -z $windows ]]; then
        "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -o out/pad-test
        out/pad-test
    else
        echo 'skipping test_pad (POSIX-only: mkdtemp/setenv); run it on Linux' >&2
    fi
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_file_mods.c "${win_compat[@]}" "${libraries[@]}" "${link[@]}" -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" "${link[@]}" -o out/sema-test
    out/sema-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
fi
