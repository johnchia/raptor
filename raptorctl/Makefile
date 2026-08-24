SRCS := raptorctl.c raptorctl_dispatch.c raptorctl_config.c raptorctl_ipc.c \
       raptorctl_help.c raptorctl_info.c
OBJS := $(SRCS:.c=.o)
BIN  := raptorctl

all: $(BIN)

# Relink when a linked-in library changes, not only when a source does: see
# LIB_HAL_VIDEO_FILE in the top-level Makefile. $(LIBS) mixes real files with
# -L/-l flags, and only the files can be a prerequisite. The build-info object
# is left out on purpose -- it is regenerated on every parse to stamp the time,
# and listing it would relink every daemon on every build.
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

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
