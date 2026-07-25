# Raptor Streaming System -- top-level build
#
# Usage:
#   make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-
#   make PLATFORM=T31 CROSS_COMPILE=mipsel-linux- rvd ringdump
#   make clean
#
# Required:
#   PLATFORM       - Target SoC:
#                      Ingenic   - T10, T20, T21, T23, T30, T31, T32, T33, T40, T41, A1
#                      SigmaStar - INFINITY6E
#   CROSS_COMPILE  - Cross-compiler prefix
#
# Optional:
#   SIGMASTAR_SDK  - SigmaStar SDK staging dir (default: ../sigmastar-sdk),
#                    laid out as <soc>/{include,lib}

ifeq ($(filter clean distclean build,$(MAKECMDGOALS)),)
ifndef PLATFORM
$(error PLATFORM not set. Use: make PLATFORM=T31 CROSS_COMPILE=mipsel-linux-)
endif
endif

# Sibling repos
HAL_DIR    := ../raptor-hal
IPC_DIR    := ../raptor-ipc
COMMON_DIR := ../raptor-common
COMPY_DIR  := ../compy

# Vendor selection — must match raptor-hal's Makefile. Ingenic parts link the
# single IMP library; SigmaStar parts link the per-module MI libraries.
SIGMASTAR_PLATFORMS := INFINITY6E
ifneq ($(filter $(PLATFORM),$(SIGMASTAR_PLATFORMS)),)
VENDOR := sigmastar
else
VENDOR := ingenic
endif

SIGMASTAR_SDK      ?= $(CURDIR)/../sigmastar-sdk
SDK_DIR_INFINITY6E := infinity6e
STAR_LIB_DIR       := $(SIGMASTAR_SDK)/$(SDK_DIR_$(PLATFORM))/lib

# Toolchain
CC     := $(CROSS_COMPILE)gcc
AR     := $(CROSS_COMPILE)ar
STRIP  := $(CROSS_COMPILE)strip

# Common flags for all daemons
EXTRA_CFLAGS ?=
CFLAGS := -Wall -Wextra -Werror
CFLAGS += -std=gnu11 -D_GNU_SOURCE
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -DPLATFORM_$(PLATFORM)
CFLAGS += -I$(CURDIR)/$(HAL_DIR)/include
CFLAGS += -I$(CURDIR)/$(IPC_DIR)/include
CFLAGS += -I$(CURDIR)/$(COMMON_DIR)/include
CFLAGS += -I$(CURDIR)/$(COMMON_DIR)/third_party/monocypher

# xburst2 (T40/T41/A1) toolchain uses -mfp64 ABI by default.
# Ensure largefile support matches buildroot target flags.
ifneq ($(filter T40 T41 A1,$(PLATFORM)),)
CFLAGS += -D_LARGEFILE_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64
endif

CFLAGS += $(EXTRA_CFLAGS)

# Build info — generate a tiny .o with string constants that each daemon links.
# Uses $(shell) so it runs at Makefile parse time, before any targets.
RSS_BUILD_HASH ?= $(shell git -C $(CURDIR) rev-parse --short HEAD 2>/dev/null || echo unknown)
RSS_BUILD_TIME ?= $(shell date -u '+%Y-%m-%dT%H:%M:%SZ')
RSS_BUILD_OBJ := $(CURDIR)/rss_build_info.o
$(shell printf 'const char *rss_build_hash = "%s";\nconst char *rss_build_time = "%s";\nconst char *rss_build_platform = "%s";\n' \
	'$(RSS_BUILD_HASH)' '$(RSS_BUILD_TIME)' '$(PLATFORM)' > $(CURDIR)/rss_build_info.c)
$(RSS_BUILD_OBJ): $(CURDIR)/rss_build_info.c
	@echo "  CC      rss_build_info.c"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@

# Every daemon links this object for the build banner
RSS_BUILD_LIBS := $(RSS_BUILD_OBJ)

ifeq ($(DEBUG),1)
CFLAGS += -O0 -g
else
# LTO does the whole-program dead-code elimination that lets daemons drop unused
# library code (e.g. libmov write-side funcs in rfs). At -O0 it doesn't, so debug
# builds omit -flto and rely on -ffunction-sections + -Wl,--gc-sections instead.
CFLAGS += -Os -flto -fno-asynchronous-unwind-tables -fmerge-all-constants -fno-ident
endif

ifeq ($(AUDIO_EFFECTS),1)
CFLAGS += -DRAPTOR_AUDIO_EFFECTS
endif

ifeq ($(AAC),1)
CFLAGS += -DRAPTOR_AAC
LDFLAGS_AAC_ENC := -lfaac
LDFLAGS_AAC_DEC := -lhelix-aac
endif

ifeq ($(MP3),1)
CFLAGS += -DRAPTOR_MP3 -DARDUINO
LDFLAGS_MP3 := -lhelix-mp3
endif

ifeq ($(OPUS),1)
CFLAGS += -DRAPTOR_OPUS
LDFLAGS_OPUS := -lopus
endif

ifeq ($(WEBTORRENT),1)
CFLAGS += -DRAPTOR_WEBTORRENT
endif

ifeq ($(V),1)
Q :=
else
Q := @
endif

# Compy include paths (for RSD only).
# The build dir is arch-suffixed because compy is a separate CMake project
# built per target; build-standalone.sh overrides COMPY_BUILD/LIB_COMPY_FILE
# outright, so this only sets the default for in-tree builds.
ifeq ($(VENDOR),sigmastar)
COMPY_BUILD  ?= $(CURDIR)/$(COMPY_DIR)/build-arm
else
COMPY_BUILD  ?= $(CURDIR)/$(COMPY_DIR)/build-mips
endif
COMPY_CFLAGS := -I$(CURDIR)/$(COMPY_DIR)/include \
                -I$(COMPY_BUILD)/_deps/slice99-src \
                -I$(COMPY_BUILD)/_deps/datatype99-src \
                -I$(COMPY_BUILD)/_deps/interface99-src \
                -I$(COMPY_BUILD)/_deps/metalang99-src/include

ifeq ($(TLS),1)
COMPY_CFLAGS += -DCOMPY_HAS_TLS
LDFLAGS_TLS := -lmbedtls -lmbedx509 -lmbedcrypto
endif

# Library file paths (for Make dependencies and build triggers)
LIB_HAL_VIDEO_FILE ?= $(CURDIR)/$(HAL_DIR)/libraptor_hal_video.a
LIB_HAL_AUDIO_FILE ?= $(CURDIR)/$(HAL_DIR)/libraptor_hal_audio.a
LIB_IPC_FILE    ?= $(CURDIR)/$(IPC_DIR)/librss_ipc.so
LIB_COMMON_FILE ?= $(CURDIR)/$(COMMON_DIR)/librss_common.so
LIB_COMPY_FILE  ?= $(COMPY_BUILD)/libcompy.a

# Library link flags (for linker command line)
LIB_HAL_VIDEO ?= $(LIB_HAL_VIDEO_FILE)
LIB_HAL_AUDIO ?= $(LIB_HAL_AUDIO_FILE)
LIB_IPC    ?= -L$(CURDIR)/$(IPC_DIR) -lrss_ipc
LIB_COMMON ?= -L$(CURDIR)/$(COMMON_DIR) -lrss_common
LIB_COMPY  ?= $(LIB_COMPY_FILE)

# TLS helper (compiled separately, only linked by daemons that need it).
# Source is in raptor-common (standalone) or sysroot (buildroot).
# Gated on TLS=1: without mbedtls in the sysroot the compile fails,
# and rhd builds HTTP-only (its sources guard on RSS_HAS_TLS).
ifeq ($(TLS),1)
RSS_TLS_SRC := $(firstword $(wildcard $(CURDIR)/$(COMMON_DIR)/src/rss_tls.c) \
                           $(wildcard $(SYSROOT)/usr/share/raptor-common/rss_tls.c))
RSS_TLS_OBJ := $(CURDIR)/rss_tls.o
RSS_TLS_CFLAGS := -DRSS_HAS_TLS
ifneq ($(RSS_TLS_SRC),)
$(RSS_TLS_OBJ): $(RSS_TLS_SRC)
	@echo "  CC      rss_tls.c"
	$(Q)$(CC) $(CFLAGS) -c $< -o $@
endif
else
RSS_TLS_OBJ :=
RSS_TLS_CFLAGS :=
endif

# Sysroot for finding shared libs (set by Buildroot or manually)
SYSROOT ?=
ifneq ($(SYSROOT),)
LDFLAGS_SYSROOT := -L$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/lib -Wl,--allow-shlib-undefined
else
LDFLAGS_SYSROOT :=
endif

# libc shim — prefer static archive (eliminates .so from device), fall back to shared
SHIM_A := $(firstword $(wildcard $(SYSROOT)/usr/lib/libmuslshim.a $(SYSROOT)/lib/libmuslshim.a \
                                 $(SYSROOT)/usr/lib/libuclibcshim.a $(SYSROOT)/lib/libuclibcshim.a))
ifneq ($(SHIM_A),)
# Static link: pull all symbols (libimp.so resolves them from the executable)
SHIM_LIB := -Wl,--whole-archive $(SHIM_A) -Wl,--no-whole-archive -Wl,--export-dynamic
else
SHIM_LIB := $(if $(wildcard $(SYSROOT)/usr/lib/libmuslshim.so $(SYSROOT)/lib/libmuslshim.so),-lmuslshim,\
             $(if $(wildcard $(SYSROOT)/usr/lib/libuclibcshim.so $(SYSROOT)/lib/libuclibcshim.so),-luclibcshim,))
endif

# libstdc++ linkage (for JZDL, SRT, rsd-555)
ifeq ($(STATIC_STDCXX),1)
LINK_STDCXX := -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic
else
LINK_STDCXX := -lstdc++
endif

# Vendor SDK libraries.
ifeq ($(VENDOR),sigmastar)
# SigmaStar splits the MI SDK into one .so per module, and each libmi_*.so
# declares only libc.so.6 as NEEDED — every cross-library symbol is left
# undefined for the loader to satisfy from whatever else is in the global
# scope. Both reference implementations sidestep this by dlopen'ing
# libcam_os_wrapper.so first with RTLD_GLOBAL (waybeam_venc's star6e_mi.c,
# divinus's i6_sys.h); linking directly instead means naming the full
# closure here, hence --allow-shlib-undefined.
#
# This set is the verified transitive closure for the video path: its only
# unresolved symbols are the usual optional/weak ones (__gmon_start__,
# _ITM_*, __stack_chk_guard). Two dependencies are non-obvious:
#   libcus3a   - libmi_isp.so calls CUS3A_* (the customer 3A entry points)
#   libispalgo - libcus3a.so in turn calls AeInit/DoAe/AwbInit/IspLoadIqCfg
#
# Audio (libmi_ai/libmi_ao) and OSD (libmi_rgn) are deliberately absent:
# they belong to later phases, and the audio libraries additionally need
# G711*/g726_*/Iaa*/MI_AED_* from vendor algorithm libraries that the
# OpenIPC osdrv package does not ship. That gap has to be resolved before
# the audio phase, so linking them now would only hide it.
VENDOR_LIBS := -L$(STAR_LIB_DIR) -Wl,-rpath-link,$(STAR_LIB_DIR) \
               -lmi_sys -lmi_vif -lmi_vpe -lmi_venc -lmi_isp -lmi_sensor \
               -lcus3a -lispalgo -lcam_os_wrapper \
               -Wl,--allow-shlib-undefined
else
VENDOR_LIBS := -limp -lalog
endif

# System libs for HAL-linked daemons
# Shim must come BEFORE the vendor SDK libs — symbols must be resolved first.
LDFLAGS_HAL := $(LDFLAGS_SYSROOT) $(SHIM_LIB) $(VENDOR_LIBS) -lpthread -lrt -lm -ldl -latomic

# IVS detection libs
ifeq ($(IVS_DETECT),1)
CFLAGS += -DIVS_DETECT
LDFLAGS_HAL += -ljzdl.m $(LINK_STDCXX)
ifeq ($(PERSONDET),1)
CFLAGS += -DPERSONDET
LDFLAGS_HAL += -lpersonDet_inf -ljzdl -Wl,--no-as-needed -lmxu_core -lmxu_imgproc -lmxu_merge -lmxu_video -Wl,--as-needed
endif
endif
LDFLAGS     := $(LDFLAGS_SYSROOT) -lpthread -lrt -latomic

# Page size: Ingenic SoCs use 4KB pages but the MIPS toolchain defaults to
# 64KB max-page-size, and mismatched alignment causes SIGBUS on musl/uclibc.
# ARM targets are 4KB too and default to a larger max-page-size, so the same
# value is correct there and additionally avoids the segment padding.
LDFLAGS_HAL += -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections -Wl,--as-needed -Wl,-rpath,/usr/lib $(if $(DEBUG),,-flto)
LDFLAGS     += -Wl,-z,max-page-size=0x1000 -Wl,--gc-sections -Wl,--as-needed -Wl,-rpath,/usr/lib $(if $(DEBUG),,-flto)
# rpath-link for local builds (finding .so at link time)
LDFLAGS_HAL += -Wl,-rpath-link,$(CURDIR)/$(IPC_DIR) -Wl,-rpath-link,$(CURDIR)/$(COMMON_DIR)
LDFLAGS     += -Wl,-rpath-link,$(CURDIR)/$(IPC_DIR) -Wl,-rpath-link,$(CURDIR)/$(COMMON_DIR)
LDFLAGS     += $(EXTRA_LDFLAGS)
EXTRA_LDFLAGS ?=
LDFLAGS_HAL += $(EXTRA_LDFLAGS)
LDFLAGS     += $(EXTRA_LDFLAGS)

# live555 include paths (for rsd-555)
LIVE555_SYSROOT ?= $(SYSROOT)
LIVE555_INC := -I$(LIVE555_SYSROOT)/usr/include/liveMedia \
               -I$(LIVE555_SYSROOT)/usr/include/groupsock \
               -I$(LIVE555_SYSROOT)/usr/include/UsageEnvironment \
               -I$(LIVE555_SYSROOT)/usr/include/BasicUsageEnvironment
LIVE555_LIBS ?= $(LIVE555_SYSROOT)/usr/lib/libliveMedia.a \
                $(LIVE555_SYSROOT)/usr/lib/libgroupsock.a \
                $(LIVE555_SYSROOT)/usr/lib/libBasicUsageEnvironment.a \
                $(LIVE555_SYSROOT)/usr/lib/libUsageEnvironment.a

# Targets
DAEMONS := rvd rsd rad rhd rod ric rmr rmd rwd rwc rfs rsp rsr rsd-555
TOOLS   := raptorctl ringdump rac rlatency rverify

.PHONY: all clean libs $(DAEMONS) $(TOOLS) install

all: libs $(DAEMONS) $(TOOLS)

# Phase 1 quick target
phase1: libs rvd ringdump raptorctl

# -- Libraries --

libs: $(LIB_HAL_VIDEO_FILE) $(LIB_HAL_AUDIO_FILE) $(LIB_IPC_FILE) $(LIB_COMMON_FILE)

# The sibling libraries are built by sub-makes, reached through a phony
# target rather than by a rule on the library files themselves.
#
# A rule whose target is the library and which carries no prerequisites runs
# only when the file is missing, so edits to a sibling's sources never reach
# the daemons here. Only the sub-make knows what its own library depends on,
# and a phony prerequisite is what hands it that decision on every build.
#
# One phony target per sibling, not one per library file: make enters a phony
# target once per run however many targets depend on it, so the raptor-hal
# sub-make cannot run twice concurrently under -j for its two archives.
#
# Each block is guarded on its sibling being present, because these rules
# describe a developer checkout, where raptor-hal, raptor-ipc and raptor-common
# sit next to this tree. A packaged build has no siblings -- each library comes
# from its own package through LIB_*_FILE pointing into the sysroot, where the
# file already exists and nothing here should try to enter a directory that is
# not there. Guarded per library rather than once for all three, because a
# mixture is legal: bisecting one library while the other two come from
# staging.
ifneq ($(wildcard $(HAL_DIR)/Makefile),)
.PHONY: build-raptor-hal
build-raptor-hal:
	@echo "  BUILD   raptor-hal"
	$(Q)$(MAKE) -C $(HAL_DIR) PLATFORM=$(PLATFORM) CROSS_COMPILE=$(CROSS_COMPILE) \
		$(if $(DEBUG),DEBUG=1,)

$(LIB_HAL_VIDEO_FILE) $(LIB_HAL_AUDIO_FILE): build-raptor-hal
	@:
endif

ifneq ($(wildcard $(IPC_DIR)/Makefile),)
.PHONY: build-raptor-ipc
build-raptor-ipc:
	@echo "  BUILD   raptor-ipc"
	$(Q)$(MAKE) -C $(IPC_DIR) CC="$(CC)"

$(LIB_IPC_FILE): build-raptor-ipc
	@:
endif

ifneq ($(wildcard $(COMMON_DIR)/Makefile),)
.PHONY: build-raptor-common
build-raptor-common:
	@echo "  BUILD   raptor-common"
	$(Q)$(MAKE) -C $(COMMON_DIR) CC="$(CC)"

$(LIB_COMMON_FILE): build-raptor-common
	@:
endif

# -- Daemons --

rvd: $(LIB_HAL_VIDEO_FILE) $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rvd"
	$(Q)$(MAKE) -C rvd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL_VIDEO) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS_HAL)" Q="$(Q)"

rsd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(LIB_COMPY_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rsd"
	$(Q)$(MAKE) -C rsd CC="$(CC)" CFLAGS="$(CFLAGS) $(COMPY_CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS)" Q="$(Q)"

rsd-555: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rsd-555"
	$(Q)$(MAKE) -C rsd-555 CC="$(CC)" CXX="$(CROSS_COMPILE)g++" CFLAGS="$(CFLAGS)" \
		LIVE555_INC="$(LIVE555_INC)" \
		LIVE555_LIBS="$(LIVE555_LIBS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" LINK_STDCXX="$(LINK_STDCXX)" Q="$(Q)"

rad: $(LIB_HAL_AUDIO_FILE) $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rad"
	$(Q)$(MAKE) -C rad CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_HAL_AUDIO) $(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS_HAL) $(LDFLAGS_AAC_ENC) $(LDFLAGS_OPUS)" Q="$(Q)"

rhd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_TLS_OBJ) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rhd"
	$(Q)$(MAKE) -C rhd CC="$(CC)" CFLAGS="$(CFLAGS) $(RSS_TLS_CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_TLS_OBJ) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS)" Q="$(Q)"

rod: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rod"
	$(Q)$(MAKE) -C rod CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) -lschrift $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ric: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   ric"
	$(Q)$(MAKE) -C ric CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) -ldl" Q="$(Q)"

rmr: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rmr"
	$(Q)$(MAKE) -C rmr CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rmd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rmd"
	$(Q)$(MAKE) -C rmd CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rwd: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(LIB_COMPY_FILE) $(RSS_TLS_OBJ) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rwd"
	$(Q)$(MAKE) -C rwd CC="$(CC)" CFLAGS="$(CFLAGS) $(COMPY_CFLAGS) -DMBEDTLS_ALLOW_PRIVATE_ACCESS -DRSS_HAS_TLS" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(LIB_COMPY) $(RSS_TLS_OBJ) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS) $(LDFLAGS_OPUS) $(LDFLAGS_AAC_DEC)" WEBTORRENT=$(WEBTORRENT) Q="$(Q)"

rwc: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rwc"
	$(Q)$(MAKE) -C rwc CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

LIBMOV_DIR := $(CURDIR)/.deps/media-server/libmov

rfs: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rfs"
	$(Q)$(MAKE) -C rfs CC="$(CC)" \
		CFLAGS="$(CFLAGS) -I$(CURDIR)/rad -I$(LIBMOV_DIR)/include -I$(LIBMOV_DIR)/source" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_AAC_ENC) $(LDFLAGS_OPUS) $(LDFLAGS_MP3) $(LDFLAGS_AAC_DEC)" \
		RAD_DIR="$(CURDIR)/rad" LIBMOV_DIR="$(LIBMOV_DIR)" Q="$(Q)"

RSP_CFLAGS := -DRSS_HAS_TLS
RSP_LDFLAGS :=
ifeq ($(AAC),1)
RSP_CFLAGS += -DRAPTOR_AAC_ENC -DRAPTOR_AAC
RSP_LDFLAGS += $(LDFLAGS_AAC_ENC) $(LDFLAGS_AAC_DEC)
endif
ifeq ($(OPUS),1)
RSP_CFLAGS += -DRAPTOR_OPUS
RSP_LDFLAGS += $(LDFLAGS_OPUS)
endif

rsp: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_TLS_OBJ) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rsp"
	$(Q)$(MAKE) -C rsp CC="$(CC)" CFLAGS="$(CFLAGS) $(RSP_CFLAGS) -I$(CURDIR)/rmr" \
		RMR_DIR="$(CURDIR)/rmr" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_TLS_OBJ) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_TLS) $(RSP_LDFLAGS)" Q="$(Q)"

LDFLAGS_SRT ?= -lsrt $(LINK_STDCXX) -latomic $(LDFLAGS_TLS)
CFLAGS_SRT  ?= $(if $(SYSROOT),-I$(SYSROOT)/usr/include)

rsr: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rsr"
	$(Q)$(MAKE) -C rsr CC="$(CC)" CFLAGS="$(CFLAGS) $(CFLAGS_SRT)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_SRT)" Q="$(Q)"

# -- Tools --

raptorctl: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   raptorctl"
	$(Q)$(MAKE) -C raptorctl CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

ringdump: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   ringdump"
	$(Q)$(MAKE) -C ringdump CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rac: $(LIB_IPC_FILE) $(LIB_COMMON_FILE) $(RSS_BUILD_OBJ)
	@echo "  BUILD   rac"
	$(Q)$(MAKE) -C rac CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_IPC) $(LIB_COMMON) $(RSS_BUILD_LIBS)" \
		LDFLAGS="$(LDFLAGS) $(LDFLAGS_MP3) $(LDFLAGS_AAC_DEC) $(LDFLAGS_OPUS)" Q="$(Q)"

rlatency:
	@echo "  BUILD   rlatency"
	$(Q)$(MAKE) -C rlatency CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LDFLAGS="$(LDFLAGS)" Q="$(Q)"

rverify: $(LIB_COMMON_FILE)
	@echo "  BUILD   rverify"
	$(Q)$(MAKE) -C rverify CC="$(CC)" CFLAGS="$(CFLAGS)" \
		LIBS="$(LIB_COMMON)" LDFLAGS="$(LDFLAGS)" Q="$(Q)"

# -- Collect binaries --

build:
	@mkdir -p build
	@for d in $(DAEMONS) $(TOOLS); do \
		if [ -f $$d/$$d ]; then cp $$d/$$d build/; fi; \
	done
	@echo "  Binaries collected in build/"

# -- Clean --

clean:
	@for d in $(DAEMONS) $(TOOLS); do \
		echo "  CLEAN   $$d"; \
		$(MAKE) -C $$d clean 2>/dev/null || true; \
	done
	rm -f rss_build_info.c rss_build_info.o rss_tls.o
	rm -rf build

distclean: clean
	$(MAKE) -C $(HAL_DIR) clean
	$(MAKE) -C $(IPC_DIR) clean 2>/dev/null || true
	$(MAKE) -C $(COMMON_DIR) clean 2>/dev/null || true

# -- Install --

install:
	install -d $(DESTDIR)/usr/bin
	install -d $(DESTDIR)/etc
	install -d $(DESTDIR)/etc/init.d
	for d in $(DAEMONS); do \
		[ -f $$d/$$d ] && install -m 0755 $$d/$$d $(DESTDIR)/usr/bin/ || true; \
	done
	for t in $(TOOLS); do \
		[ -f $$t/$$t ] && install -m 0755 $$t/$$t $(DESTDIR)/usr/bin/ || true; \
	done
	install -m 0644 config/raptor.conf $(DESTDIR)/etc/raptor.conf
	install -m 0755 config/S31raptor $(DESTDIR)/etc/init.d/S31raptor
