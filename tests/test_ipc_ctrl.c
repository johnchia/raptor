/*
 * test_ipc_ctrl.c -- a control round trip has to come back
 *
 * Every caller of the control socket passes a timeout and believes it. It
 * covered the read and nothing else: connect() was blocking with no bound at
 * all, so a daemon that was running, listening and not accepting would hold
 * the caller for as long as it stayed that way.
 *
 * That is not the failure it looks like from the outside. A daemon that has
 * exited leaves no socket and connect() fails at once, which is the case
 * everything was tested against. A daemon that is stopped, wedged, or busy in
 * something long still has its listener, so the kernel queues the connection
 * instead -- and once the backlog is full, the next caller waits. rcd's serve
 * loop is single-threaded, so a wait there is the camera's whole configuration
 * interface.
 *
 * The probe runs on its own thread and is joined against a deadline, so a
 * regression fails this test rather than hanging the suite -- there is no
 * per-test timeout, and a test that can only be observed by the run never
 * finishing is not much of a test.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "greatest.h"

#include <rss_common.h>
#include <rss_ipc.h>

#define WEDGED_SOCK RSS_RUN_DIR "/wedged-test.sock"

/* A listener that never accepts: the daemon is there, and it is not
 * answering. Nothing else reproduces the case that matters. */
static int wedged_listen(void)
{
	struct sockaddr_un a;
	int fd;

	mkdir(RSS_RUN_DIR, 0755);
	unlink(WEDGED_SOCK);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;

	memset(&a, 0, sizeof(a));
	a.sun_family = AF_UNIX;
	rss_strlcpy(a.sun_path, WEDGED_SOCK, sizeof(a.sun_path));

	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

typedef struct {
	int rc;
	int calls;
} probe_t;

static void *probe_run(void *arg)
{
	probe_t *p = arg;

	/*
	 * Enough calls to fill the listener's backlog and then ask once more.
	 * The ones that fit are answered by the timeout on the read; the one
	 * that does not is the one that used to wait forever.
	 */
	for (int i = 0; i < 12; i++) {
		char *resp = NULL;

		p->rc = rss_ctrl_send_command_alloc(WEDGED_SOCK, "{\"cmd\":\"ping\"}", &resp, 200);
		free(resp);
		p->calls++;
	}
	return NULL;
}

TEST a_daemon_that_never_accepts_does_not_hold_the_caller_forever(void)
{
	int lfd = wedged_listen();

	if (lfd < 0)
		SKIPm("cannot listen in " RSS_RUN_DIR " -- run the suite under unshare -rm");

	probe_t p = {0};
	pthread_t tid;

	ASSERT_EQ(0, pthread_create(&tid, NULL, probe_run, &p));

	/*
	 * Twelve calls at a 200 ms budget is 2.4 s of honest waiting. Ten
	 * seconds is loose enough that a loaded build machine does not fail
	 * it, and finite, which is the whole assertion.
	 */
	struct timespec until;

	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_sec += 10;

	int jr = pthread_timedjoin_np(tid, NULL, &until);

	if (jr != 0) {
		/* Left running deliberately: it is blocked in a call that does
		 * not return, which is the thing being reported. */
		pthread_detach(tid);
		close(lfd);
		unlink(WEDGED_SOCK);
		FAILm("a control round trip against a listening, non-accepting daemon never "
		      "came back");
	}

	close(lfd);
	unlink(WEDGED_SOCK);

	ASSERT_EQm("not every call was made", 12, p.calls);

	/* And they failed rather than silently succeeding against a daemon
	 * that answered nothing. */
	ASSERTm("a wedged daemon reported success", p.rc < 0);
	PASS();
}

SUITE(ipc_ctrl_suite)
{
	RUN_TEST(a_daemon_that_never_accepts_does_not_hold_the_caller_forever);
}
