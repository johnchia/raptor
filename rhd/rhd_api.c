/*
 * rhd_api.c -- the one route that carries configuration
 *
 * See rhd_api.h for why this file knows nothing about keys.
 */

#include <crypt.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

#include "rhd.h"
#include "rhd_api.h"
#include "rhd_authrate.h"

#include <rss_shadow.h>

/*
 * One round trip, owned by two threads.
 *
 * The client can go away -- a browser tab closing mid-apply is ordinary --
 * while the worker is still blocked in a read on rcd's socket. Neither side
 * may free what the other is holding, so both hold a reference and the last
 * one out frees. Nothing here points back at the client for the same reason.
 */
typedef struct rhd_api_job {
	pthread_mutex_t lock;
	int refs;
	bool done;
	char *req;  /* request body, owned */
	char *resp; /* rcd's reply, owned; NULL when the round trip failed */
	char err[96];
} rhd_api_job_t;

static void job_release(rhd_api_job_t *job)
{
	pthread_mutex_lock(&job->lock);
	int left = --job->refs;
	pthread_mutex_unlock(&job->lock);
	if (left > 0)
		return;
	pthread_mutex_destroy(&job->lock);
	free(job->req);
	free(job->resp);
	free(job);
}

static void *api_worker(void *arg)
{
	rhd_api_job_t *job = arg;
	char *resp = NULL;

	/* Returns the reply's length, or a negative errno. */
	int rc = rss_ctrl_send_command_alloc(RSS_RUN_DIR "/rcd.sock", job->req, &resp,
					     RHD_API_TIMEOUT_MS);

	pthread_mutex_lock(&job->lock);
	if (rc >= 0 && resp) {
		job->resp = resp;
	} else {
		free(resp);
		/* The daemon, not the request: rcd refuses in JSON and that
		 * reply reaches the browser untouched. Getting here means it
		 * did not answer at all. */
		snprintf(job->err, sizeof(job->err), "rcd did not answer (%d)", rc);
	}
	job->done = true;
	pthread_mutex_unlock(&job->lock);

	job_release(job);
	return NULL;
}

/* ── request framing ── */

static const char *header_value(const char *buf, const char *name, size_t *len_out)
{
	size_t nlen = strlen(name);
	const char *p = buf;

	/* Header lines only: stop at the blank line so a body cannot supply a
	 * header the sender did not. */
	const char *end = strstr(buf, "\r\n\r\n");
	while (p && (!end || p < end)) {
		if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
			p += nlen + 1;
			while (*p == ' ' || *p == '\t')
				p++;
			const char *eol = strstr(p, "\r\n");
			if (!eol)
				return NULL;
			*len_out = (size_t)(eol - p);
			return p;
		}
		p = strstr(p, "\r\n");
		if (!p)
			break;
		p += 2;
	}
	return NULL;
}

static long content_length(const char *buf)
{
	size_t vlen = 0;
	const char *v = header_value(buf, "Content-Length", &vlen);
	if (!v)
		return -1;
	char tmp[24];
	if (vlen >= sizeof(tmp))
		return -1;
	memcpy(tmp, v, vlen);
	tmp[vlen] = '\0';
	return strtol(tmp, NULL, 10);
}

bool rhd_request_complete(const char *buf, size_t len)
{
	const char *end = strstr(buf, "\r\n\r\n");
	if (!end)
		return false;

	long clen = content_length(buf);
	if (clen <= 0)
		return true;

	size_t header_len = (size_t)(end - buf) + 4;
	return len >= header_len + (size_t)clen;
}

/* ── the route ── */

/*
 * The api's error body, from the serializer.
 *
 * The shape is fixed and the fields are not: `reason` is a sentence somebody
 * wrote, and at one call site it is rcd's own words relayed through. A quote
 * in there produced a body no client could parse, so cJSON puts the strings
 * in. Answers 0 on allocation failure, which the callers send as an empty
 * body -- already how they report a daemon that said nothing.
 */
static int api_error_body(char *buf, size_t cap, const char *code, const char *reason)
{
	cJSON *r = cJSON_CreateObject();
	int n = 0;

	if (!r)
		return 0;
	buf[0] = '\0';
	cJSON_AddNumberToObject(r, "api", 1);
	cJSON_AddStringToObject(r, "status", "error");
	cJSON_AddStringToObject(r, "code", code);
	cJSON_AddStringToObject(r, "reason", reason ? reason : "");
	/* One byte held back for the newline every one of these has always
	 * ended with; a reader tailing the socket by line still sees them. */
	if (cJSON_PrintPreallocated(r, buf, (int)cap - 2, 0)) {
		n = (int)strlen(buf);
		buf[n++] = '\n';
		buf[n] = '\0';
	}
	cJSON_Delete(r);
	return n;
}

static void api_refuse(rhd_client_t *c, const char *status, const char *code, const char *reason)
{
	char body[256];
	int n = api_error_body(body, sizeof(body), code, reason);

	http_send(c, status, "application/json", body, n);
}

/* -- who may configure the camera -- */

/*
 * This route authenticates against the system account, and never against
 * [http] username/password.
 *
 * Those are the media credential. They are handed to an NVR, to Home
 * Assistant, to whoever is allowed to watch, and rhd sends them back in
 * cleartext on every snapshot -- so they are the credential that leaks. This
 * route rewrites the network stanza and restarts the pipeline, which is not
 * something a viewing password may reach. /etc/shadow holds the only secret
 * on the camera that is not also handed out to watch video.
 *
 * A realm of its own, so that once a browser has been challenged here it
 * holds the system account for this route and not the media credential. It
 * still offers the media credential first: RFC 7617 lets a client try what it
 * holds for the page on everything under the page's URL, and the page is at
 * the root. So a name that is not the system account's is refused without
 * being charged as a guess -- see api_authenticate.
 */
#define RHD_API_REALM "Raptor Config"

/*
 * crypt() answers in static storage, which is safe here only because this runs
 * on the main loop thread and nothing else in rhd calls it. The comparison is
 * constant-time so a wrong password does not leak how much of it was right.
 */
static bool system_account_ok(const char *user, const char *pass)
{
	char hash[160];

	if (!rss_shadow_hash(RHD_SHADOW_PATH, user, hash, sizeof(hash)))
		return false;

	const char *got = crypt(pass, hash);

	return got && rss_secure_compare(got, hash);
}

/*
 * Duplicated from rhd_main.c rather than shared through rhd.h, because that
 * file is upstream's and this one is not: five lines here cost less than a
 * conflict there.
 */
static int64_t api_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * A username on its way to the log, which is attacker-supplied text going
 * somewhere an operator will read. Printable ASCII only and short: a name
 * carrying a newline could otherwise forge a second log line, and one
 * carrying terminal escapes could rewrite what the reader sees.
 */
static void auth_safe_name(const char *in, char *out, size_t outsz)
{
	size_t n = 0;

	for (; in[n] && n + 1 < outsz; n++)
		out[n] = (in[n] >= 0x20 && in[n] < 0x7f) ? in[n] : '?';
	out[n] = '\0';
	if (n == 0)
		rss_strlcpy(out, "(empty)", outsz);
}

typedef enum {
	API_AUTH_NONE,	    /* nothing offered -- no hash was computed */
	API_AUTH_THROTTLED, /* offered, but this host is paying for earlier guesses */
	API_AUTH_BAD,	    /* offered and wrong -- one hash was computed */
	API_AUTH_OK,
} api_auth_t;

static api_auth_t api_authenticate(const char *request, const char *host, int *retry_sec)
{
	size_t vlen = 0;
	const char *v = header_value(request, "Authorization", &vlen);

	if (!v || vlen <= 6 || strncasecmp(v, "Basic ", 6) != 0)
		return API_AUTH_NONE;
	v += 6;
	vlen -= 6;

	char decoded[256];
	int dlen = rss_base64_decode(v, vlen, decoded, sizeof(decoded) - 1);

	if (dlen <= 0)
		return API_AUTH_NONE;
	decoded[dlen] = '\0';

	char *colon = strchr(decoded, ':');

	if (!colon)
		return API_AUTH_NONE;
	*colon = '\0';

	/*
	 * A name that is not the system account's cannot be a right answer,
	 * so it costs no hash and no strike. It is not a guess: it is what a
	 * browser holding the media credential for the page offers here on
	 * its own, on every request, and charged as a wrong password the
	 * console's own polling locked its operator out within a minute of
	 * setting one.
	 */
	if (strcmp(decoded, RHD_API_USER) != 0) {
		char name[64];

		auth_safe_name(decoded, name, sizeof(name));
		RSS_DEBUG("api: %s offered '%s', which is not the system account", host, name);
		return API_AUTH_BAD;
	}

	int64_t now = api_now_ms();

	if (!rhd_auth_may_hash(host, now, retry_sec))
		return API_AUTH_THROTTLED;

	if (system_account_ok(decoded, colon + 1)) {
		rhd_auth_succeeded(host, now);
		return API_AUTH_OK;
	}

	rhd_auth_failed(host, now);

	/* The name that was tried, which the old line left out -- so a log
	 * showing a hundred of these says whether somebody is working through
	 * usernames or has one account's password slightly wrong. */
	char name[64];

	auth_safe_name(decoded, name, sizeof(name));
	RSS_WARN("api: %s offered the wrong system password for '%s'", host, name);
	return API_AUTH_BAD;
}

/*
 * Too many wrong answers. A separate code from 401 on purpose: an operator
 * who has mistyped their password wants to be told they are being made to
 * wait, not handed the same rejection again. Retry-After says how long, and
 * saying so gives away nothing an attacker cannot measure anyway.
 */
static void api_429(rhd_client_t *c, int retry_sec)
{
	char body[256];
	int blen = api_error_body(body, sizeof(body), "auth",
				  "too many failed attempts; wait and try again");
	char header[256];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 429 Too Many Requests\r\n"
			    "Retry-After: %d\r\n"
			    "Content-Type: application/json\r\n"
			    "Content-Length: %d\r\n"
			    "Connection: close\r\n"
			    "\r\n",
			    retry_sec > 0 ? retry_sec : 1, blen);

	rhd_write(c, header, (size_t)hlen);
	rhd_write(c, body, (size_t)blen);
}

static void api_401(rhd_client_t *c)
{
	char body[256];
	int blen = api_error_body(body, sizeof(body), "auth",
				  "the configuration api needs the system account");
	char header[256];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 401 Unauthorized\r\n"
			    "WWW-Authenticate: Basic realm=\"" RHD_API_REALM "\"\r\n"
			    "Content-Type: application/json\r\n"
			    "Content-Length: %d\r\n"
			    "Connection: close\r\n"
			    "\r\n",
			    blen);

	rhd_write(c, header, (size_t)hlen);
	rhd_write(c, body, (size_t)blen);
}

/*
 * Hand a request to a worker and answer from rhd_api_poll() when it comes
 * back. `req` is copied, so a caller that built it may free its own.
 *
 * Shared by both routes below because both round trips are the same round
 * trip: what differs is who was allowed to start one and what the body is
 * permitted to say, and neither of those is this function's business.
 */
static bool api_start(rhd_client_t *c, const char *req, size_t len)
{
	if (len > RHD_API_MAX_BODY) {
		api_refuse(c, "413 Payload Too Large", "too-many", "request body too large");
		return true;
	}
	if (c->api_job) {
		api_refuse(c, "409 Conflict", "busy", "a request is already in flight");
		return true;
	}

	rhd_api_job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		api_refuse(c, "503 Service Unavailable", "io", "out of memory");
		return true;
	}
	job->req = malloc(len + 1);
	if (!job->req) {
		free(job);
		api_refuse(c, "503 Service Unavailable", "io", "out of memory");
		return true;
	}
	memcpy(job->req, req, len);
	job->req[len] = '\0';
	pthread_mutex_init(&job->lock, NULL);
	job->refs = 2; /* this client, and the worker about to start */

	pthread_t tid;
	if (pthread_create(&tid, NULL, api_worker, job) != 0) {
		job->refs = 1;
		job_release(job);
		api_refuse(c, "503 Service Unavailable", "io", "cannot start a worker");
		return true;
	}
	pthread_detach(tid);

	c->api_job = job;
	return true;
}

/*
 * The body of a POST, once the checks both routes share have passed. Returns
 * false having already answered the client.
 *
 * Insist on the JSON content type. A form-encoded or text/plain POST is a
 * request a browser will send cross-origin without asking first; this one it
 * must preflight, and rhd answers no preflight. That is the whole of the
 * cross-site story here, so it is not optional -- and it matters most on the
 * claim route, which by construction answers a camera with no password to
 * check.
 */
static bool api_post_body(rhd_client_t *c, const char **body, size_t *blen)
{
	size_t ctlen = 0;
	const char *ct = header_value(c->recv_buf, "Content-Type", &ctlen);

	if (!ct || ctlen < 16 || strncasecmp(ct, "application/json", 16) != 0) {
		api_refuse(c, "415 Unsupported Media Type", "malformed",
			   "content-type must be application/json");
		return false;
	}

	const char *end = strstr(c->recv_buf, "\r\n\r\n");
	if (!end) {
		api_refuse(c, "400 Bad Request", "malformed", "no request body");
		return false;
	}
	const char *b = end + 4;
	size_t n = c->recv_len - (size_t)(b - c->recv_buf);
	long clen = content_length(c->recv_buf);

	if (clen > 0 && (size_t)clen < n)
		n = (size_t)clen;
	if (n == 0) {
		api_refuse(c, "400 Bad Request", "malformed", "no request body");
		return false;
	}
	*body = b;
	*blen = n;
	return true;
}

/* -- claiming -- */

/*
 * Whether this camera may still be taken, which is one bit and is answered to
 * anybody who asks.
 *
 * The console has to know which card to draw before it has a credential to
 * draw it with, so this cannot be behind the authentication it is the way out
 * of. What it gives away is a bit an attacker establishes anyway by sending a
 * claim and reading the refusal; publishing it costs nothing and saves the
 * page from inferring its state from an error.
 */
static void api_claim_state(rhd_client_t *c)
{
	rss_shadow_state_t st = rss_shadow_state(RHD_SHADOW_PATH, RHD_API_USER);
	cJSON *r = cJSON_CreateObject();
	char body[128];

	if (!r) {
		api_refuse(c, "500 Internal Server Error", "io", "out of memory");
		return;
	}
	cJSON_AddStringToObject(r, "status", "ok");
	cJSON_AddBoolToObject(r, "claimed", st == RSS_SHADOW_SET);
	cJSON_AddBoolToObject(r, "claimable", st == RSS_SHADOW_UNSET);

	bool ok = cJSON_PrintPreallocated(r, body, (int)sizeof(body), 0);

	cJSON_Delete(r);
	if (!ok) {
		api_refuse(c, "500 Internal Server Error", "io", "could not serialise");
		return;
	}
	http_send(c, "200 OK", "application/json", body, (int)strlen(body));
}

/*
 * Take the camera: set the root password on one that has none.
 *
 * Two things happen here and nowhere else in this file. The route answers
 * without authentication, because the credential it would ask for is the one
 * it is about to create. And the request that reaches rcd is built here rather
 * than forwarded, from one field, so that an unauthenticated caller cannot
 * name a command -- forwarding this body verbatim would be an open door onto
 * every verb rcd has.
 *
 * rcd refuses an already-claimed camera as well. That is not redundancy for
 * its own sake: this is the only unauthenticated write on the device, the two
 * checks read the same file from different processes, and one check is one
 * mistake.
 */
static bool api_claim(rhd_client_t *c, const char *method)
{
	if (strcmp(method, "GET") == 0) {
		api_claim_state(c);
		return true;
	}
	if (strcmp(method, "POST") != 0) {
		api_refuse(c, "405 Method Not Allowed", "malformed", "claiming takes GET or POST");
		return true;
	}

	char host[64];

	client_addr_str(&c->addr, host, sizeof(host));

	/*
	 * 403 and not 401, for all three refusals. An operator who mistyped a
	 * password and a camera that is not theirs to take are different
	 * problems, and answering both with the same challenge sends the first
	 * one looking for a password that would not have helped.
	 */
	switch (rss_shadow_state(RHD_SHADOW_PATH, RHD_API_USER)) {
	case RSS_SHADOW_UNSET:
		break;
	case RSS_SHADOW_SET:
		RSS_WARN("claim: %s tried to claim a camera that is already claimed", host);
		api_refuse(c, "403 Forbidden", "claimed", "this camera has already been claimed");
		return true;
	case RSS_SHADOW_LOCKED:
		RSS_WARN("claim: %s asked to claim a camera whose root account is locked", host);
		api_refuse(c, "403 Forbidden", "claimed",
			   "this camera's root account is locked, so it cannot be claimed "
			   "over the network");
		return true;
	case RSS_SHADOW_MISSING:
		RSS_WARN("claim: %s asked to claim a camera with no readable %s", host,
			 RHD_SHADOW_PATH);
		api_refuse(c, "403 Forbidden", "io", "this camera has no account to claim");
		return true;
	}

	const char *body = NULL;
	size_t blen = 0;

	if (!api_post_body(c, &body, &blen))
		return true;

	cJSON *in = cJSON_ParseWithLength(body, blen);
	const cJSON *pw = in ? cJSON_GetObjectItemCaseSensitive(in, "password") : NULL;

	if (!cJSON_IsString(pw) || !pw->valuestring) {
		cJSON_Delete(in);
		api_refuse(c, "400 Bad Request", "malformed", "claiming needs a 'password'");
		return true;
	}

	/*
	 * Rebuilt rather than passed through, and only the one field is
	 * carried across. Whether the value is a password this camera will
	 * accept is rcd's table's answer, not this file's -- the grammar,
	 * the length and the pre-derived form are all decided there.
	 */
	cJSON *req = cJSON_CreateObject();
	if (!req || !cJSON_AddStringToObject(req, "cmd", "claim") ||
	    !cJSON_AddStringToObject(req, "password", pw->valuestring)) {
		cJSON_Delete(req);
		cJSON_Delete(in);
		api_refuse(c, "503 Service Unavailable", "io", "out of memory");
		return true;
	}

	char *wire = cJSON_PrintUnformatted(req);

	cJSON_Delete(req);
	cJSON_Delete(in);
	if (!wire) {
		api_refuse(c, "503 Service Unavailable", "io", "out of memory");
		return true;
	}

	/*
	 * Logged at warning level, with the address, and before the answer is
	 * known. An owner who plugs a camera in and finds it already claimed
	 * has one question, and this is the line that answers it.
	 */
	RSS_WARN("claim: %s is claiming this camera", host);

	bool taken = api_start(c, wire, strlen(wire));

	free(wire);
	return taken;
}

bool rhd_api_handle(rhd_server_t *srv, rhd_client_t *c, const char *method, const char *path)
{
	if (strcmp(path, RHD_API_CLAIM_PATH) == 0) {
		if (!srv->api_enabled) {
			api_refuse(c, "403 Forbidden", "unknown", "the api is disabled");
			return true;
		}
		return api_claim(c, method);
	}

	if (strcmp(path, RHD_API_PATH) != 0)
		return false;

	if (strcmp(method, "POST") != 0) {
		api_refuse(c, "405 Method Not Allowed", "malformed", "the api takes POST");
		return true;
	}
	if (!srv->api_enabled) {
		api_refuse(c, "403 Forbidden", "unknown", "the api is disabled");
		return true;
	}

	/*
	 * After the enabled check, so a camera with the api switched off says
	 * so rather than asking for credentials it will refuse anyway.
	 *
	 * Setup mode does not authenticate -- but only while there is nothing
	 * to authenticate against. The reason the portal is open is that the
	 * owner of a camera being set up for the first time has not been given
	 * a credential yet, and asking for one would refuse everybody
	 * including them. The moment the camera is claimed that reason is
	 * gone: a password exists, so a portal that still asked for nothing
	 * would be an open configuration surface on an open access point --
	 * which is exactly what a camera that fell back to setup mode after a
	 * wifi failure would be. What bounds the remaining exposure is the
	 * network: the listener answers only on the access point the camera
	 * raised. See rhd_portal.h.
	 *
	 * Nothing narrows *what* may be set here, deliberately. rcd's table
	 * is the policy, and an allow-list in this file would be a second
	 * copy of it -- the thing this whole route exists not to have.
	 */
	bool claimed = rss_shadow_state(RHD_SHADOW_PATH, RHD_API_USER) == RSS_SHADOW_SET;

	if (!srv->portal || claimed) {
		char host[64];
		int retry_sec = 1;

		client_addr_str(&c->addr, host, sizeof(host));

		switch (api_authenticate(c->recv_buf, host, &retry_sec)) {
		case API_AUTH_OK:
			break;
		case API_AUTH_THROTTLED:
			api_429(c, retry_sec);
			return true;
		case API_AUTH_NONE:
		case API_AUTH_BAD:
			/* Unlogged here. A request carrying no credentials is
			 * what every browser sends first, and a line for each
			 * of those buries the ones that mean something; the
			 * wrong-password case has already logged itself, with
			 * the name that was tried. */
			api_401(c);
			return true;
		}
	}

	const char *body = NULL;
	size_t blen = 0;

	if (!api_post_body(c, &body, &blen))
		return true;

	return api_start(c, body, blen);
}

void rhd_api_release(rhd_client_t *c)
{
	if (!c->api_job)
		return;
	job_release((rhd_api_job_t *)c->api_job);
	c->api_job = NULL;
}

bool rhd_api_waiting(const rhd_server_t *srv)
{
	for (int i = 0; i < srv->client_count; i++)
		if (srv->clients[i]->api_job)
			return true;
	return false;
}

void rhd_api_poll(rhd_server_t *srv)
{
	for (int i = 0; i < srv->client_count; i++) {
		rhd_client_t *c = srv->clients[i];
		rhd_api_job_t *job = (rhd_api_job_t *)c->api_job;
		if (!job)
			continue;

		pthread_mutex_lock(&job->lock);
		bool done = job->done;
		char *resp = job->resp;
		char err[sizeof(job->err)];
		rss_strlcpy(err, job->err, sizeof(err));
		pthread_mutex_unlock(&job->lock);

		if (!done)
			continue;

		if (resp) {
			/* rcd's own envelope, byte for byte. A refusal is a
			 * 200 carrying status "error": the HTTP status says
			 * whether the camera answered, the body says what it
			 * decided, and conflating the two costs the client the
			 * `code` it wants to act on. */
			http_send_async_ex(c, srv->epoll_fd, "application/json", resp,
					   (uint32_t)strlen(resp), false);
		} else {
			char body[256];
			int n = api_error_body(body, sizeof(body), "daemon",
					       err[0] ? err : "no answer from rcd");

			http_send_async_ex(c, srv->epoll_fd, "application/json", body, (uint32_t)n,
					   false);
		}

		rhd_api_release(c);
	}
}
