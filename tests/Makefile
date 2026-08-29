CC ?= gcc

# Sanitizer selection — `address` (default) or `thread`. ASAN and TSAN
# are mutually exclusive; CI runs both in separate jobs.
# Sanitizer and language flags shared with build-asan.sh: 24 of the
# sources below are also compiled there, and while the two disagreed
# these tests ran without UBSan over code the daemons ran with it.
include ../sanitizer-flags.mk

SAN ?= address
ifeq ($(SAN),thread)
SAN_FLAGS = $(RAPTOR_SAN_THREAD)
else
SAN_FLAGS = $(RAPTOR_SAN_ADDRESS)
endif

CFLAGS = -Wall -Wextra -std=$(RAPTOR_STD) -D_GNU_SOURCE -g $(SAN_FLAGS) $(RAPTOR_SAN_EXTRA) \
         -I../rmr -I../ric -I../rwd -I../rsp -I../rvd -I../rcd -I../../raptor-hal/include \
         -I../../raptor-common/include -I../../raptor-common/third_party/monocypher \
         -I../../raptor-ipc/include -I.
LDFLAGS = $(SAN_FLAGS) -lpthread -lrt -lm

TEST_SRCS = main.c test_ric_json.c test_nal.c test_prebuf.c test_mux.c test_codec.c test_sdp.c test_ring.c test_rsp.c \
            test_ctrl.c test_sign.c test_storage.c test_timelapse.c test_rcd_cmd.c \
            test_rhd_auth.c ../rhd/rhd_authrate.c test_rhd_slots.c test_ipc_ctrl.c \
            test_sendq.c test_idr_recovery.c test_rwd_sendq.c test_rwd_pacer.c test_rwd_rtcp.c \
            test_rad_clock.c test_media_clock.c test_resync.c test_vui.c \
            test_backchannel.c test_config.c test_ring_size.c ../rvd/rvd_ring_size.c
LIB_SRCS = ../ric/ric_json.c ../rsd/rsd_sendq.c ../rsd/rsd_backchannel.c ../rwd/rwd_sendq.c ../rwd/rwd_pacer.c ../rad/rad_clock.c ../rad/rad_resync.c ../rmr/rmr_nal.c ../rmr/rmr_prebuf.c ../rmr/rmr_mux.c ../rmr/rmr_sign.c ../rmr/rmr_timelapse.c ../../raptor-common/src/rss_media_clock.c ../../raptor-common/src/rss_vui.c ../../raptor-common/src/rss_sign.c ../../raptor-common/src/rss_jpeg.c ../../raptor-common/src/rss_aac.c ../rmr/rmr_storage.c \
           ../../raptor-common/src/rss_util.c ../../raptor-common/src/rss_log.c \
           ../../raptor-common/src/rss_daemon.c \
           ../../raptor-common/src/cJSON.c \
           ../../raptor-common/third_party/monocypher/monocypher.c \
           ../../raptor-common/third_party/monocypher/monocypher-ed25519.c
CTRL_SRCS = ../rvd/rvd_ctrl.c mock_hal.c \
            ../../raptor-common/src/rss_config.c ../../raptor-common/src/rss_ctrl_cmds.c \
            ../../raptor-common/src/rss_ctrl_client.c

# rcd owns the configuration policy: what may be set, to what, by whom, and at
# what cost. Every source but rcd_main.c, which carries main() -- the rest link
# clean without a socket or a config file, which is the point of the split.
RCD_SRCS = ../rcd/rcd_config.c ../rcd/rcd_schema.c ../rcd/rcd_proto.c \
           ../rcd/rcd_system.c ../rcd/rcd_guard.c ../rcd/rcd_network.c ../rcd/rcd_wifi.c \
           ../rcd/rcd_apply.c ../rcd/rcd_state.c ../rcd/rcd_ipc.c
IPC_SRCS = ../../raptor-ipc/src/rss_ring.c ../../raptor-ipc/src/rss_ctrl.c \
           ../../raptor-ipc/src/rss_osd_shm.c ../../raptor-ipc/src/rss_ipc_log.c

# rwd_sdp.c includes rwd.h which pulls in compy+mbedTLS — compile it with
# a minimal shim header instead (only needs rwd_sdp_offer_t + rss_strlcpy).
SDP_SRC = sdp_parse.c
$(SDP_SRC): ../rwd/rwd_sdp.c
	sed '/^int rwd_sdp_generate_answer/,$$d' $< > $@

# rcd's [system] providers write real files -- /etc/TZ, /etc/ntp.conf. Pointed
# at a scratch directory under the tmpfs the suite already mounts on /run, so
# the tests exercise the writers themselves rather than a mock of them, and a
# suite run outside that sandbox fails to write rather than editing this host.
RCD_TEST_ETC = /run/rss/rcd-test-etc

# Headers too. The suite compiles every source in one command, so there are no
# object files to carry -MMD dependencies -- which meant a header change left
# the binary silently stale, and a test asserting something about a header went
# on asserting the old value. Found the hard way, twice.
TEST_HDRS = $(wildcard *.h ../*/*.h ../../raptor-hal/include/*.h \
                       ../../raptor-common/include/*.h ../../raptor-ipc/include/*.h)

tests: $(TEST_SRCS) $(LIB_SRCS) $(CTRL_SRCS) $(IPC_SRCS) $(RCD_SRCS) $(SDP_SRC) $(TEST_HDRS)
	$(CC) $(CFLAGS) -DRWD_H -DRCD_SYSCONF_DIR='"$(RCD_TEST_ETC)"' \
		-include ../fuzz/fuzz_rwd_shim.h \
		-o $@ $(TEST_SRCS) $(LIB_SRCS) $(CTRL_SRCS) $(IPC_SRCS) $(RCD_SRCS) $(SDP_SRC) $(LDFLAGS)

test_slow_rtsp: test_slow_rtsp.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: tests console portal
	./tests

# The console is JavaScript in one HTML file and nothing else here can run it.
# A thin DOM shim draws every tab, which is the check worth having: a TypeError
# anywhere in render() empties the whole page rather than spoiling one row.
# Skipped rather than failed where node is not installed -- CI has it, a
# bring-up bench may not.
console:
	@if command -v node > /dev/null 2>&1; then \
		node console_smoke.js; \
	else \
		echo "console_smoke: node not found, skipped"; \
	fi

# The setup page carries its own PBKDF2-HMAC-SHA1, because the portal is served
# over plain HTTP by design and crypto.subtle does not exist there. A wrong
# derivation looks exactly like a mistyped passphrase from the camera's side,
# so it is checked against known vectors rather than by reading it.
portal:
	@if command -v node > /dev/null 2>&1; then \
		node portal_smoke.js; \
	else \
		echo "portal_smoke: node not found, skipped"; \
	fi

clean:
	rm -f tests test_slow_rtsp sdp_parse.c

.PHONY: test console portal clean
