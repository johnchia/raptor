#!/bin/sh
# Raptor full rebuild script
# Usage: ./build.sh <platform> <br_output> [target...]
# Examples:
#   ./build.sh t31 /path/to/buildroot/output
#   ./build.sh t20 /path/to/buildroot/output rvd rsd
#   ./build.sh t31 /path/to/buildroot/output clean
#   ./build.sh infinity6e /path/to/openipc-firmware/output rod

set -e

platform="$1"
br_output="$2"
if [ $# -ge 2 ]; then
    shift 2
fi

case "$platform" in
    t10|T10) PLATFORM=T10 ;;
    t20|T20) PLATFORM=T20 ;;
    t21|T21) PLATFORM=T21 ;;
    t23|T23) PLATFORM=T23 ;;
    t30|T30) PLATFORM=T30 ;;
    t31|T31) PLATFORM=T31 ;;
    t32|T32) PLATFORM=T32 ;;
    t33|T33) PLATFORM=T33 ;;
    t40|T40) PLATFORM=T40 ;;
    t41|T41) PLATFORM=T41 ;;
    a1|A1)   PLATFORM=A1 ;;
    infinity6e|INFINITY6E|ssc30kq|SSC30KQ) PLATFORM=INFINITY6E ;;
    infinity6b0|INFINITY6B0|ssc333|SSC333|ssc335|SSC335|ssc337|SSC337) PLATFORM=INFINITY6B0 ;;
    infinity6c|INFINITY6C|ssc377|SSC377|ssc377de|SSC377DE|ssc378|SSC378|ssc379|SSC379)
        PLATFORM=INFINITY6C ;;
    *)
        echo "Usage: $0 <platform> <br_output> [target...]"
        echo "Platforms: t10 t20 t21 t23 t30 t31 t32 t33 t40 t41 a1 infinity6e infinity6b0 infinity6c"
        echo ""
        echo "  <br_output> is the buildroot output directory containing"
        echo "  host/ with the cross-compiler and sysroot."
        exit 1
        ;;
esac

# Everything above is Ingenic and mipsel; the Infinity6 families are SigmaStar
# and ARM. That splits both the sysroot tuple and the compiler prefix, so
# neither can stay hardcoded below.
case "$PLATFORM" in
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
        # Either uClibc or glibc, and unlike the other two families that is not
        # a contradiction. This family's vendor blobs declare a libc outright,
        # and the drop ships the whole MI set twice -- one build needing
        # libc.so.0 and one needing libc.so.6 -- so the C library is a property
        # of the image rather than of the chip. Both tuples are searched because
        # either can be the right answer; what matters is that the toolchain
        # matches the MI set the image carries, and mismatching it is not a link
        # error but a loader that never finds a libc the libraries can use.
        #
        # Where Infinity6E is forced to glibc by its 4.9 kernel, this family
        # runs 5.10 and a uClibc toolchain is buildable against it, which is
        # what makes the smaller set an option at all.
        #
        # Only one of these exists in any given Buildroot output, so the search
        # order decides nothing.
        SYSROOT_TUPLES="arm-thingino-linux-uclibcgnueabihf arm-buildroot-linux-uclibcgnueabihf \
                        arm-openipc-linux-uclibcgnueabihf arm-thingino-linux-gnueabihf \
                        arm-buildroot-linux-gnueabihf arm-openipc-linux-gnueabihf"
        CROSS_CANDIDATES="arm-thingino-linux-uclibcgnueabihf- \
                          arm-buildroot-linux-uclibcgnueabihf- arm-linux-uclibcgnueabihf- \
                          arm-thingino-linux-gnueabihf- arm-buildroot-linux-gnueabihf- \
                          arm-linux-gnueabihf-"
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
