# The browse itself lives in rmq and is compiled here from there rather than
# copied: the probe is only evidence about rmq if it is the same code.
VPATH := ../rmq

SRCS := mdnsprobe.c rmq_mdns.c
OBJS := $(SRCS:.c=.o)
BIN  := mdnsprobe

all: $(BIN)

$(BIN): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CC) -o $@ $^ $(LIBS) $(LDFLAGS)

%.o: %.c
	@echo "  CC      $<"
	$(Q)$(CC) $(CFLAGS) -I../rmq -MMD -MP -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(BIN)

-include $(OBJS:.o=.d)

.PHONY: all clean
