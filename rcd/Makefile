SRCS := rcd_main.c rcd_proto.c rcd_schema.c rcd_config.c rcd_apply.c rcd_state.c rcd_ipc.c \
        rcd_system.c rcd_guard.c rcd_network.c
OBJS := $(SRCS:.c=.o)
BIN  := rcd

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $^ $(LIBS) $(LDFLAGS)

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
