CXX    ?= $(CROSS_COMPILE)g++
CC     ?= $(CROSS_COMPILE)gcc

SRCS_C   := rsd555_queue.c rsd555_reader.c rsd555_ctrl.c
SRCS_CXX := rsd555_main.cpp rsd555_source.cpp rsd555_subsession.cpp rsd555_backchannel.cpp

OBJS := $(SRCS_C:.c=.o) $(SRCS_CXX:.cpp=.o)
BIN  := rsd-555

# live555 include paths (set by parent Makefile or standalone build)
LIVE555_INC ?=
LIVE555_LIBS ?=

CXXFLAGS_LOCAL := $(filter-out -std=gnu11,$(CFLAGS)) -std=c++20 \
	-DNO_OPENSSL -DNO_STD_LIB \
	$(LIVE555_INC)
CFLAGS_LOCAL   := $(CFLAGS)

all: $(BIN)

# Relink when a linked-in library changes, not only when a source does: see
# LIB_HAL_VIDEO_FILE in the top-level Makefile. The parent passes the authoritative
# list, because $(LIBS) reaches some libraries as -L/-l flags rather than as paths
# and only a path can be a prerequisite; this assignment is the fallback for a
# standalone sub-make, and a command-line LIB_DEPS overrides it. The build-info
# object is left out on purpose -- it is regenerated on every parse to stamp the
# time, and listing it would relink every daemon on every build.
LIB_DEPS := $(filter %.a %.so,$(LIBS) $(LIVE555_LIBS))

# Recompile when the platform selection changes, not only when a source does:
# see BUILD_CONFIG in the top-level Makefile. Empty for a standalone sub-make,
# which then behaves exactly as before.
CONFIG_STAMP ?=
$(OBJS): $(CONFIG_STAMP)

$(BIN): $(OBJS) $(LIB_DEPS)
	@echo "  LD      $@"
	$(Q)$(CXX) -o $@ $(OBJS) $(LIBS) $(LIVE555_LIBS) $(LDFLAGS) $(LINK_STDCXX)

%.o: %.cpp
	@echo "  CXX     $<"
	$(Q)$(CXX) $(CXXFLAGS_LOCAL) -MMD -MP -c $< -o $@

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS_LOCAL) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
