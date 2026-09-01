#!/bin/sh
# Raptor full rebuild script
# Usage: ./build.sh <platform> <br_output> [target...]
# Examples:
#   ./build.sh t31 /path/to/buildroot/output
#   ./build.sh t20 /path/to/buildroot/output rvd rsd
#   ./build.sh t31 /path/to/buildroot/output clean
#   ./build.sh infinity6e /path/to/openipc-firmware/output rod
#   ./build.sh hi3516ev300 /path/to/openipc-firmware/output

set -e

platform="$1"
br_output="$2"
if [ $# -ge 2 ]; then
    shift 2
fi

# Folded to lower case once rather than spelling every arm twice: the
# SigmaStar list is twenty part numbers, and SSC337DE/ssc337DE/ssc337de are
# the same part however someone types it.
platform_lc=$(echo "$platform" | tr '[:upper:]' '[:lower:]')

case "$platform_lc" in
    t10) PLATFORM=T10 ;;
    t20) PLATFORM=T20 ;;
    t21) PLATFORM=T21 ;;
    t23) PLATFORM=T23 ;;
    t30) PLATFORM=T30 ;;
    t31) PLATFORM=T31 ;;
    t32) PLATFORM=T32 ;;
    t33) PLATFORM=T33 ;;
    t40) PLATFORM=T40 ;;
    t41) PLATFORM=T41 ;;
    a1)  PLATFORM=A1 ;;
    # Naming a family builds for the family and leaves the per-part caps
    # fields unmeasured; naming a part fills them in. The two are separate
    # arms because the part is not readable at runtime -- the chip ID is
    # family-wide, and ipctool reports SSC33X for ssc333/335/337 alike --
    # so this argument is the only place the part can come from.
    #
    # The part lists are the vendor's, and the spellings are the ones
    # OpenIPC's defconfigs already use for BR2_OPENIPC_SOC_MODEL, so a board
    # can pass its own value straight through. Variants that differ only in
    # package or DRAM still appear: they are separate parts to name, and
    # src/caps_sigmastar.inc is where they get grouped by encoder rate.
    infinity6b0) PLATFORM=INFINITY6B0 ;;
    ssc333|ssc333de|ssc335|ssc335de|ssc337|ssc337de)
        PLATFORM=INFINITY6B0; SOC_MODEL=$platform_lc ;;
    infinity6e) PLATFORM=INFINITY6E ;;
    ssc336d|ssc336q|ssc30kd|ssc30kq|ssc338d|ssc338q|ssc338g|ssc339g)
        PLATFORM=INFINITY6E; SOC_MODEL=$platform_lc ;;
    infinity6c) PLATFORM=INFINITY6C ;;
    ssc377|ssc377d|ssc377de|ssc377qe|ssc378de|ssc378qe)
        PLATFORM=INFINITY6C; SOC_MODEL=$platform_lc ;;
    # HiSilicon gen4. No family/part split here: the part IS the platform,
    # because unlike SigmaStar the chip ID is readable and part-specific
    # (0x12020EE0 reads 0x3516E300 on an EV300), so nothing has to be passed
    # in that the board cannot answer for itself.
    hi3516ev200) PLATFORM=HI3516EV200 ;;
    hi3516ev300) PLATFORM=HI3516EV300 ;;
    *)
        echo "Usage: $0 <platform> <br_output> [target...]"
        echo ""
        echo "Families: t10 t20 t21 t23 t30 t31 t32 t33 t40 t41 a1"
        echo "          infinity6b0 infinity6e infinity6c"
        echo "          hi3516ev200 hi3516ev300"
        echo ""
        echo "Parts. Naming one of these instead of its family fills in the"
        echo "per-part encoder ceilings, which a family build leaves unset:"
        echo "  infinity6b0  ssc333 ssc333de ssc335 ssc335de ssc337 ssc337de"
        echo "  infinity6e   ssc336d ssc336q ssc30kd ssc30kq"
        echo "               ssc338d ssc338q ssc338g ssc339g"
        echo "  infinity6c   ssc377 ssc377d ssc377de ssc377qe"
        echo "               ssc378de ssc378qe"
        echo ""
        echo "  <br_output> is the buildroot output directory containing"
        echo "  host/ with the cross-compiler and sysroot."
        exit 1
        ;;
esac

# Everything above is Ingenic and mipsel; the Infinity6 families are SigmaStar
# and ARM, and the Hi3516 parts are HiSilicon and ARM. That splits both the
# sysroot tuple and the compiler prefix, so neither can stay hardcoded below.
case "$PLATFORM" in
    HI3516EV200|HI3516EV300)
        # Soft-float, and note the tuples: musleabi, NOT musleabihf. This is
        # the one ARM family here that is not hard-float, so it gets its own
        # arm rather than a label on Infinity6E's.
        #
        # Measured rather than assumed: Tag_ABI_VFP_args is absent from every
        # binary on a stock OpenIPC hi3516ev300 image -- libmpi, libisp,
        # libsecurec, the six lib_hi*.so algorithm libraries, all 34
        # libsns_*.so, and majestic itself -- while Tag_FP_arch reads VFPv4.
        # The FPU is used; the calling convention is not.
        #
        # Getting this wrong is not a link error. A hard-float daemon links,
        # loads and runs, and hands garbage to every float argument crossing
        # into MPI. raptor-hal's v4_common.h carries a #error on __ARM_PCS_VFP
        # so it cannot survive a compile either.
        SYSROOT_TUPLES="arm-openipc-linux-musleabi arm-buildroot-linux-musleabi \
                        arm-thingino-linux-musleabi"
        CROSS_CANDIDATES="arm-openipc-linux-musleabi- arm-buildroot-linux-musleabi- \
                          arm-linux-musleabi-"
        CROSS_GLOB="arm"
        ;;
    INFINITY6E)
        SYSROOT_TUPLES="arm-buildroot-linux-gnueabihf arm-openipc-linux-gnueabihf \
                        arm-thingino-linux-gnueabihf arm-buildroot-linux-musleabihf"
        # OpenIPC's Buildroot installs arm-openipc-*-gcc against an
        # arm-buildroot-* sysroot, so the prefix and the tuple are not the
        # same string and the prefix has to be looked for separately.
        CROSS_CANDIDATES="arm-openipc-linux-gnueabihf- arm-buildroot-linux-gnueabihf- \
                          arm-linux-gnueabihf-"
        CROSS_GLOB="arm"
        ;;
    INFINITY6B0)
        # musl, not glibc, and the distinction is fatal rather than cosmetic:
        # there is no glibc anywhere in an Infinity6B0 image, so a
        # gnueabihf-linked daemon does not reach its entry point. The families
        # split this way because the vendor blobs do -- Infinity6E's
        # libmi_sys.so declares NEEDED libc.so.6, while Infinity6B0's declare
        # no libc at all and resolve plain POSIX out of the loading process.
        #
        # Only standalone builds can get this wrong. Through Buildroot the
        # toolchain comes from the camera defconfig, which already knows.
        SYSROOT_TUPLES="arm-buildroot-linux-musleabihf arm-openipc-linux-musleabihf \
                        arm-thingino-linux-musleabihf"
        CROSS_CANDIDATES="arm-openipc-linux-musleabihf- arm-buildroot-linux-musleabihf- \
                          arm-linux-musleabihf-"
        CROSS_GLOB="arm"
        ;;
    INFINITY6C)
        # uClibc, glibc or musl, and unlike the other two families that is not
        # a contradiction. This family's vendor blobs declare a libc outright,
        # and the drop ships the whole MI set more than once -- one build
        # needing libc.so.0 and one needing libc.so.6 -- so the C library is a
        # property of the image rather than of the chip. All three are searched
        # because any of them can be the right answer; what matters is that the
        # toolchain matches the MI set the image carries, and mismatching it is
        # not a link error but a loader that never finds a libc the libraries
        # can use.
        #
        # Where Infinity6E is forced to glibc by its 4.9 kernel, this family
        # runs 5.10, so the smaller libcs are buildable against it at all.
        # musl is here because an OpenIPC base is one: its output carries
        # arm-buildroot-linux-musleabihf, and every board-verified Infinity6C
        # daemon so far was built with that toolchain.
        #
        # Only one of these exists in any given Buildroot output, so the search
        # order decides nothing.
        SYSROOT_TUPLES="arm-thingino-linux-uclibcgnueabihf arm-buildroot-linux-uclibcgnueabihf \
                        arm-openipc-linux-uclibcgnueabihf arm-thingino-linux-gnueabihf \
                        arm-buildroot-linux-gnueabihf arm-openipc-linux-gnueabihf \
                        arm-buildroot-linux-musleabihf arm-openipc-linux-musleabihf \
                        arm-thingino-linux-musleabihf"
        CROSS_CANDIDATES="arm-thingino-linux-uclibcgnueabihf- \
                          arm-buildroot-linux-uclibcgnueabihf- arm-linux-uclibcgnueabihf- \
                          arm-thingino-linux-gnueabihf- arm-buildroot-linux-gnueabihf- \
                          arm-linux-gnueabihf- arm-openipc-linux-musleabihf- \
                          arm-buildroot-linux-musleabihf- arm-linux-musleabihf-"
        CROSS_GLOB="arm"
        ;;
    *)
        SYSROOT_TUPLES="mipsel-buildroot-linux-uclibc mipsel-thingino-linux-uclibc \
                        mipsel-buildroot-linux-musl mipsel-thingino-linux-musl"
        CROSS_CANDIDATES="mipsel-linux-"
        CROSS_GLOB="mipsel"
        ;;
esac

if [ -z "$br_output" ] || [ ! -d "$br_output" ]; then
    echo "Error: buildroot output directory required"
    echo "Usage: $0 <platform> <br_output> [target...]"
    exit 1
fi

TOOLCHAIN="$br_output/host/bin"

# Auto-detect sysroot tuple (uclibc or musl on Ingenic, gnueabihf on ARM)
SYSROOT=""
for tuple in $SYSROOT_TUPLES; do
    if [ -d "$br_output/host/$tuple/sysroot" ]; then
        SYSROOT="$br_output/host/$tuple/sysroot"
        break
    fi
done

if [ ! -d "$TOOLCHAIN" ]; then
    echo "Toolchain not found: $TOOLCHAIN"
    exit 1
fi

if [ -z "$SYSROOT" ]; then
    echo "Sysroot not found in $br_output/host/"
    # Unquoted on purpose: word-splitting collapses the line continuations
    # in the list above into single spaces.
    echo "  looked for:" $SYSROOT_TUPLES
    exit 1
fi

# Named candidates first so a working setup keeps the prefix it already used,
# then fall back to whatever <arch>*-gcc the toolchain actually installed.
CROSS_COMPILE=""
for c in $CROSS_CANDIDATES; do
    if [ -x "$TOOLCHAIN/${c}gcc" ]; then
        CROSS_COMPILE="$c"
        break
    fi
done
if [ -z "$CROSS_COMPILE" ]; then
    for gcc in "$TOOLCHAIN/$CROSS_GLOB"*-gcc; do
        [ -x "$gcc" ] || continue
        CROSS_COMPILE="$(basename "$gcc" gcc)"
        break
    done
fi

if [ -z "$CROSS_COMPILE" ]; then
    echo "No $CROSS_GLOB cross-compiler found in $TOOLCHAIN"
    echo "  looked for:" $CROSS_CANDIDATES
    exit 1
fi

export PATH="$TOOLCHAIN:$PATH"

MAKE_ARGS="PLATFORM=$PLATFORM CROSS_COMPILE=$CROSS_COMPILE SYSROOT=$SYSROOT AAC=1 OPUS=1 MP3=1"
[ -n "${SOC_MODEL:-}" ] && MAKE_ARGS="$MAKE_ARGS SOC_MODEL=$SOC_MODEL"

# Auto-detect TLS support
if [ -f "$SYSROOT/usr/lib/libmbedtls.so" ] || [ -f "$SYSROOT/lib/libmbedtls.so" ]; then
    MAKE_ARGS="$MAKE_ARGS TLS=1 WEBTORRENT=1"
fi

echo "Building for $PLATFORM"
echo "  Output:  $br_output"
echo "  Sysroot: $SYSROOT"
echo "  Cross:   $CROSS_COMPILE"

if [ $# -eq 0 ]; then
    make $MAKE_ARGS distclean
    make -j$(nproc) $MAKE_ARGS rvd rsd rad rhd rod ric rmr rmd rwd raptorctl ringdump rac
    exec make $MAKE_ARGS build
else
    exec make -j$(nproc) $MAKE_ARGS "$@"
fi
