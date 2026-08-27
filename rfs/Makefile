SRCS := rfs_main.c rfs_annexb.c rfs_mp4.c
OBJS := $(SRCS:.c=.o)
BIN  := rfs

# Audio codecs -- reuse RAD's codec plugins
RAD_DIR ?= ../rad
VPATH   += $(RAD_DIR)
CODEC_OBJS := rad_codec.o rad_codec_l16.o rad_codec_g711.o rad_codec_aac.o rad_codec_opus.o
OBJS += $(CODEC_OBJS)

# libmov MP4 demuxer
LIBMOV_DIR ?= ../.deps/media-server/libmov
LIBMOV_SRCS := $(filter-out %-writer.c %fmp4-writer.c, $(wildcard $(LIBMOV_DIR)/source/*.c))
LIBMOV_OBJS := $(addprefix mov_, $(notdir $(LIBMOV_SRCS:.c=.o)))
OBJS += $(LIBMOV_OBJS)

all: $(BIN)

# Relink when a linked-in library changes, not only when a source does: see
# LIB_HAL_VIDEO_FILE in the top-level Makefile. The parent passes the authoritative
# list, because $(LIBS) reaches some libraries as -L/-l flags rather than as paths
# and only a path can be a prerequisite; this assignment is the fallback for a
# standalone sub-make, and a command-line LIB_DEPS overrides it. The build-info
# object is left out on purpose -- it is regenerated on every parse to stamp the
# time, and listing it would relink every daemon on every build.
LIB_DEPS := $(filter %.a %.so,$(LIBS))

# Recompile when the platform selection changes, not only when a source does:
# see BUILD_CONFIG in the top-level Makefile. Empty for a standalone sub-make,
# which then behaves exactly as before.
CONFIG_STAMP ?=
$(OBJS): $(CONFIG_STAMP)

$(BIN): $(OBJS) $(LIB_DEPS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $(OBJS) $(LIBS) $(LDFLAGS)

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# RAD codec sources
rad_%.o: $(RAD_DIR)/rad_%.c
	@echo "  CC      rad_$*.c"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# libmov sources (prefix with mov_ to avoid name collisions)
mov_%.o: $(LIBMOV_DIR)/source/%.c
	@echo "  CC      $*.c (libmov)"
	$(Q)$(CC) $(CFLAGS) -w -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
