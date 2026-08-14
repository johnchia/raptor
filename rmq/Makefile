SRCS := rmq_main.c rmq_mqtt.c rmq_poll.c rmq_ha.c rmq_cmd.c rmq_restart.c rmq_snapshot.c rmq_system.c rmq_mdns.c
OBJS := $(SRCS:.c=.o)
BIN  := rmq

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
