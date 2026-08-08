# Toolchain

Installed and verified on Main Mac (Apple Silicon, macOS 26.5) 2026-08-08.

## What is installed

| Component | Version | Location |
|---|---|---|
| Zephyr | v4.3.1 | `~/zephyrproject/zephyr` |
| Zephyr SDK | 0.17.4 (arm-zephyr-eabi) | `~/zephyr-sdk-0.17.4` |
| west | 1.5.0 | `~/zephyrproject/.venv` |
| CMake | 4.4.2 | Homebrew |
| Ninja | 1.13.2 | Homebrew |
| dtc | 1.8.1 | Homebrew |

Homebrew packages: `cmake ninja gperf ccache dtc libmagic wget`.

## Environment for a fresh shell

```sh
export PATH="/opt/homebrew/bin:$PATH"
source ~/zephyrproject/.venv/bin/activate
export ZEPHYR_BASE=~/zephyrproject/zephyr
export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.4
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
```

## Two gotchas, both of which cost a build

**1. `ZEPHYR_TOOLCHAIN_VARIANT` must be set explicitly.** Without it the
build dies in `cmake/modules/FindZephyr-sdk.cmake:57` with "Unknown
arguments specified". The line is:

```cmake
if(("zephyr" STREQUAL ${ZEPHYR_TOOLCHAIN_VARIANT}) OR ...
```

The expansion is unquoted, so an unset variable leaves `if("zephyr"
STREQUAL )`, which is a CMake syntax error rather than a helpful message.
Setting the variable to `zephyr` is the fix.

**2. The source path must not contain a space.** Zephyr's devicetree
preprocessing splits the overlay list on whitespace, so a path like
`~/Documents/Other Creations/...` fails with:

```
fatal error: /Users/morten/Documents/Other: No such file or directory
```

A symlink does NOT help: west and CMake resolve it back to the physical
path. The source tree has to physically live somewhere without spaces.
Host unit tests (`make -C tests/host test`) are unaffected, since they are
plain clang.

## Toolchain proof: a byte-identical reproduction

Building `chattock/sp1-tape-looper` unmodified from a space-free path
produces a binary that is **byte-for-byte identical** to the `sp1_looper.bin`
shipped in that repo:

```
d926854d751236e0ac21445828c7ed39  build/zephyr/zephyr.bin
d926854d751236e0ac21445828c7ed39  sp1_looper.bin
BYTE-IDENTICAL
```

That is a stronger check than a size comparison: it proves the compiler,
the SDK, the board files, the Zephyr version and the UAC2 patch all match
what the upstream author used. If a future build stops matching, something
in the toolchain has moved.

Build footprint for reference (the looper, not this project): FLASH 108200
bytes of 892 KB (11.85%), RAM 255956 bytes of 256 KB (97.64%). The RAM
figure is the looper's audio buffers; this firmware will use a fraction.

Steps to reproduce:

```sh
# one-time: patch the Zephyr tree for the looper's USB audio
cd ~/zephyrproject/zephyr
git apply /path/to/sp1-remote/refs/sp1-tape-looper/zephyr-patches/uac2-windows-fs-feedback.patch

# build from a space-free path
cp -R /path/to/sp1-remote/refs/sp1-tape-looper ~/sp1work/looper
cd ~/sp1work/looper
west build -p -b stem_player firmware -- -DBOARD_ROOT="$HOME/sp1work/looper"
```

Note that this project's own firmware does NOT need the UAC2 patch: it has
no USB audio. The patch is only required to reproduce the looper.

## Consequence for Phase 1

Because the reproduction is byte-identical, the v0 flash test does not need
a self-built image at all: `refs/sp1-tape-looper/sp1_looper.bin` is exactly
what this toolchain produces. Flash the shipped file.
