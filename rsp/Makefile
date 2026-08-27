SRCS := rsp_main.c rsp_rtmp.c rsp_audio.c rsp_net.c
OBJS := $(SRCS:.c=.o) rmr_nal.o
BIN  := rsp

RMR_DIR ?= ../rmr
CFLAGS += -I$(RMR_DIR)

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

rmr_nal.o: $(RMR_DIR)/rmr_nal.c
	@echo "  CC      rmr_nal.c"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
