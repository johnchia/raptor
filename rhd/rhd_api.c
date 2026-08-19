/*
 * rhd_api.c -- the one route that carries configuration
 *
 * See rhd_api.h for why this file knows nothing about keys.
 */

#include <crypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

#include "rhd.h"
#include "rhd_api.h"

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

static void api_refuse(rhd_client_t *c, const char *status, const char *code, const char *reason)
{
	char body[256];
	int n = snprintf(body, sizeof(body),
			 "{\"api\":1,\"status\":\"error\",\"code\":\"%s\","
			 "\"reason\":\"%s\"}\n",
			 code, reason);
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
 * A realm of its own, so a browser holding the media credential does not
 * offer it here and then cache the rejection against it.
 */
#define RHD_API_REALM "Raptor Config"

/*
 * The account's password hash as /etc/shadow spells it.
 *
 * Every way of not finding one is the same answer: unknown user, unreadable
 * file, locked account. A locked or password-less account ("*", "!", "!!", or
 * an empty field) must never authenticate -- clearing the root password is a
 * thing people do while debugging, and it has to lock the camera rather than
 * open it to everyone.
 */
static bool shadow_hash(const char *user, char *out, size_t outsz)
{
	if (!user || !user[0] || strchr(user, ':'))
		return false;

	FILE *f = fopen("/etc/shadow", "r");
	if (!f)
		return false;

	char line[512];
	size_t ulen = strlen(user);
	bool found = false;

	while (!found && fgets(line, sizeof(line), f)) {
		if (strncmp(line, user, ulen) != 0 || line[ulen] != ':')
			continue;

		char *hash = line + ulen + 1;
		char *end = strchr(hash, ':');

		if (end)
			*end = '\0';
		hash[strcspn(hash, "\r\n")] = '\0';

		/*
		 * A usable hash is "$id$salt$digest". Nothing else is accepted,
		 * which also rules out a bare DES hash -- no account here has
		 * one, and refusing an unrecognised field is the safe way to be
		 * wrong.
		 */
		if (hash[0] == '$' && strlen(hash) < outsz) {
			rss_strlcpy(out, hash, outsz);
			found = true;
		}
		break;
	}

	fclose(f);
	return found;
}

/*
 * crypt() answers in static storage, which is safe here only because this runs
 * on the main loop thread and nothing else in rhd calls it. The comparison is
 * constant-time so a wrong password does not leak how much of it was right.
 */
static bool system_account_ok(const char *user, const char *pass)
{
	char hash[128];

	if (!shadow_hash(user, hash, sizeof(hash)))
		return false;

	const char *got = crypt(pass, hash);

	return got && rss_secure_compare(got, hash);
}

static bool api_authenticated(const char *request)
{
	size_t vlen = 0;
	const char *v = header_value(request, "Authorization", &vlen);

	if (!v || vlen <= 6 || strncasecmp(v, "Basic ", 6) != 0)
		return false;
	v += 6;
	vlen -= 6;

	char decoded[256];
	int dlen = rss_base64_decode(v, vlen, decoded, sizeof(decoded) - 1);

	if (dlen <= 0)
		return false;
	decoded[dlen] = '\0';

	char *colon = strchr(decoded, ':');

	if (!colon)
		return false;
	*colon = '\0';

	return system_account_ok(decoded, colon + 1);
}

static void api_401(rhd_client_t *c)
{
	static const char body[] = "{\"api\":1,\"status\":\"error\",\"code\":\"auth\","
				   "\"reason\":\"the configuration api needs the system "
				   "account\"}\n";
	char header[256];
	int hlen = snprintf(header, sizeof(header),
			    "HTTP/1.1 401 Unauthorized\r\n"
			    "WWW-Authenticate: Basic realm=\"" RHD_API_REALM "\"\r\n"
			    "Content-Type: application/json\r\n"
			    "Content-Length: %d\r\n"
			    "Connection: close\r\n"
			    "\r\n",
			    (int)strlen(body));

	rhd_write(c, header, (size_t)hlen);
	rhd_write(c, body, strlen(body));
}

bool rhd_api_handle(rhd_server_t *srv, rhd_client_t *c, const char *method, const char *path)
{
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
	 */
	if (!api_authenticated(c->recv_buf)) {
		RSS_WARN("api: refused a request with no system account");
		api_401(c);
		return true;
	}

	/*
	 * Insist on the JSON content type. A form-encoded or text/plain POST
	 * is a request a browser will send cross-origin without asking first;
	 * this one it must preflight, and rhd answers no preflight. That is
	 * the whole of the cross-site story here, so it is not optional.
	 */
	size_t ctlen = 0;
	const char *ct = header_value(c->recv_buf, "Content-Type", &ctlen);
	if (!ct || ctlen < 16 || strncasecmp(ct, "application/json", 16) != 0) {
		api_refuse(c, "415 Unsupported Media Type", "malformed",
			   "content-type must be application/json");
		return true;
	}

	const char *end = strstr(c->recv_buf, "\r\n\r\n");
	if (!end) {
		api_refuse(c, "400 Bad Request", "malformed", "no request body");
		return true;
	}
	const char *body = end + 4;
	size_t blen = c->recv_len - (size_t)(body - c->recv_buf);
	long clen = content_length(c->recv_buf);
	if (clen > 0 && (size_t)clen < blen)
		blen = (size_t)clen;

	if (blen == 0) {
		api_refuse(c, "400 Bad Request", "malformed", "no request body");
		return true;
	}
	if (blen > RHD_API_MAX_BODY) {
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
	job->req = malloc(blen + 1);
	if (!job->req) {
		free(job);
		api_refuse(c, "503 Service Unavailable", "io", "out of memory");
		return true;
	}
	memcpy(job->req, body, blen);
	job->req[blen] = '\0';
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
			char body[192];
			int n = snprintf(body, sizeof(body),
					 "{\"api\":1,\"status\":\"error\",\"code\":\"daemon\","
					 "\"reason\":\"%s\"}\n",
					 err[0] ? err : "no answer from rcd");
			http_send_async_ex(c, srv->epoll_fd, "application/json", body, (uint32_t)n,
					   false);
		}

		rhd_api_release(c);
	}
}
