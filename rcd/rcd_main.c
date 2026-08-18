/*
 * rcd_main.c -- RCD (Raptor Config Daemon)
 *
 * A control socket and a loop around it. Everything rcd does is a reply to a
 * request; it originates nothing, polls nothing on a timer, and holds no
 * hardware. The one exception is the deferred save, which is a write-back of
 * changes that are already in effect.
 */

#include "rcd.h"
#include "rcd_config.h"
#include "rcd_guard.h"
#include "rcd_proto.h"
#include "rcd_schema.h"

#include <rss_common.h>
#include <rss_ipc.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <cJSON.h>

/* Long enough that an idle camera is not spinning, short enough that a
 * deferred save lands close to when it fell due. */
#define IDLE_TICK_MS 200

static void serve_loop(rcd_state_t *st)
{
	int ctrl_fd = rss_ctrl_get_fd(st->ctrl);

	while (rss_running(st->running)) {
		fd_set fds;
		struct timeval tv = {0, IDLE_TICK_MS * 1000};
		FD_ZERO(&fds);
		FD_SET(ctrl_fd, &fds);

		if (select(ctrl_fd + 1, &fds, NULL, NULL, &tv) > 0)
			rss_ctrl_accept_and_handle(st->ctrl, rcd_handle, st);

		uint64_t now = (uint64_t)(rss_timestamp_us() / 1000);
		if (rcd_save_due(st, now))
			rcd_save_flush(st);

		/* The one thing rcd does that nobody asked it to. It is the
		 * point: the change being timed is one whose client may never
		 * come back. */
		rcd_guard_tick(st, now);
	}
}

int main(int argc, char **argv)
{
	rss_daemon_ctx_t dctx;
	int ret = rss_daemon_init(&dctx, "rcd", argc, argv, NULL);
	if (ret != 0)
		return ret > 0 ? 0 : 1;

	rcd_state_t st;
	memset(&st, 0, sizeof(st));
	st.config_path = dctx.config_path;
	st.running = dctx.running;

	/*
	 * rcd holds no copy of the configuration. Every read loads the file or
	 * asks the daemon that owns the value, and every write starts from a
	 * fresh load -- so nothing here can go stale against a file that other
	 * daemons also write, and there is no cache to invalidate.
	 */
	rss_config_free(dctx.cfg);

	rss_mkdir_p(RSS_RUN_DIR);
	st.ctrl = rss_ctrl_listen(RSS_RUN_DIR "/rcd.sock");
	if (!st.ctrl) {
		RSS_FATAL("control socket failed; rcd is nothing without it");
		rss_daemon_cleanup("rcd");
		return 1;
	}

	rcd_stale_load(&st);
	rcd_guard_load(&st);

	RSS_INFO("rcd: config %s, api %d", st.config_path, RCD_API_VERSION);

	serve_loop(&st);

	RSS_INFO("rcd shutting down");

	/* A live change made moments before the stop is still owed to flash,
	 * and the daemon holding it may be going down with us. */
	rcd_save_flush(&st);

	rss_ctrl_destroy(st.ctrl);
	rss_daemon_cleanup("rcd");
	return 0;
}
