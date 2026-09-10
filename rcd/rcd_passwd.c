/*
 * rcd_passwd.c -- Writing the root password. See rcd_passwd.h.
 */

#include <ctype.h>
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <rss_common.h>
#include <rss_shadow.h>

#include "rcd.h"
#include "rcd_config.h"
#include "rcd_proto.h"
#include "rcd_passwd.h"
#include "rcd_system.h"

#define PATH_SHADOW RCD_SYSCONF_DIR "/shadow"

/*
 * sha256-crypt, and the choice is not about the hash's strength.
 *
 * rhd runs one crypt(3) per authenticated API request, on the loop that also
 * serves MJPEG, and caches nothing -- so whatever is written here is paid
 * again on every poll the console makes. Measured on an SSC377QE: 4 ms for
 * md5crypt, 32 ms for this, 91 ms for sha512. md5crypt is not a defensible
 * choice for a new password in 2026 and sha512 puts a tenth of a second
 * between two frames several times a minute, so the middle one is the answer
 * until rhd learns to hash once per session rather than once per request.
 *
 * A pre-derived value arrives in whatever form the client chose and is stored
 * as it came: this constant is what rcd picks when it is hashing, not a rule
 * about what the field may hold.
 */
#define PASSWD_METHOD	"$5$"
#define PASSWD_SALT_LEN 16

bool rcd_passwd_claimed(void)
{
	return rss_shadow_state(PATH_SHADOW, RCD_PASSWD_USER) == RSS_SHADOW_SET;
}

bool rcd_passwd_claimable(void)
{
	return rss_shadow_state(PATH_SHADOW, RCD_PASSWD_USER) == RSS_SHADOW_UNSET;
}

/*
 * A setting string for crypt(3): the method, then salt from the kernel.
 *
 * There is no fallback if /dev/urandom cannot be read. A salt derived from the
 * clock on a camera whose clock starts at the epoch is a salt every unit of a
 * production run shares, which is the one property a salt exists to deny --
 * so a camera that cannot get random bytes does not get a password set.
 */
static bool crypt_setting(char *out, size_t outsz)
{
	static const char alphabet[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
				       "abcdefghijklmnopqrstuvwxyz";
	unsigned char raw[PASSWD_SALT_LEN];
	FILE *f = fopen("/dev/urandom", "rb");

	if (!f)
		return false;
	bool ok = fread(raw, 1, sizeof(raw), f) == sizeof(raw);
	fclose(f);
	if (!ok)
		return false;

	if (outsz < sizeof(PASSWD_METHOD) + PASSWD_SALT_LEN)
		return false;
	rss_strlcpy(out, PASSWD_METHOD, outsz);

	size_t n = strlen(out);
	for (size_t i = 0; i < PASSWD_SALT_LEN; i++)
		out[n + i] = alphabet[raw[i] % (sizeof(alphabet) - 1)];
	out[n + PASSWD_SALT_LEN] = '\0';
	return true;
}

/*
 * crypt(3) answers in static storage, which is safe here for the reason it is
 * safe in rhd: rcd serves on one thread and nothing else in it calls crypt.
 */
static bool hash_password(const char *plain, char *out, size_t outsz)
{
	char setting[sizeof(PASSWD_METHOD) + PASSWD_SALT_LEN];

	if (!crypt_setting(setting, sizeof(setting))) {
		RSS_WARN("passwd: no salt available; the password was not set");
		return false;
	}

	const char *h = crypt(plain, setting);

	/*
	 * A libc that does not implement the method answers with something
	 * that is not a hash -- historically a 13-character DES string
	 * beginning with the setting's first two bytes, which would silently
	 * store a password truncated to eight characters. Refusing anything
	 * that does not come back in the format asked for is what catches it.
	 */
	if (!h || strncmp(h, PASSWD_METHOD, strlen(PASSWD_METHOD)) != 0 ||
	    strlen(h) <= strlen(setting) || strlen(h) >= outsz) {
		RSS_WARN("passwd: this libc did not produce a " PASSWD_METHOD " hash");
		return false;
	}
	rss_strlcpy(out, h, outsz);
	return true;
}

/*
 * Replace one account's password field, carrying every other byte of the file
 * through.
 *
 * Rewriting /etc/shadow from a template would be the shorter code and would
 * drop every account rcd does not know about, along with the ageing fields on
 * the one it does. The same trap as the eth0 stanza's hwaddress line: a writer
 * that owns a file it did not author has to edit it, not reproduce it.
 */
static int shadow_write(const char *user, const char *hash)
{
	/*
	 * The field the caller is asking to store, checked against the store
	 * rather than against the grammar that produced it. A ':' would add a
	 * field and a control byte would end the record, and either leaves a
	 * file that no login parses back into the password just set -- an
	 * unrecoverable camera, from one malformed value.
	 *
	 * rcd_config.c refuses both already. This is the same reasoning as the
	 * two claim checks in two processes: the value arriving here has been
	 * hashed by one path and passed through verbatim by another, and the
	 * invariant belongs to the file, so the file's writer states it too.
	 */
	for (const char *p = hash; *p; p++) {
		if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7e || *p == ':') {
			RSS_WARN("passwd: refusing a password field %s cannot hold", PATH_SHADOW);
			return -1;
		}
	}

	FILE *in = fopen(PATH_SHADOW, "r");
	if (!in) {
		RSS_WARN("passwd: cannot read %s: %s", PATH_SHADOW, strerror(errno));
		return -1;
	}

	char tmp[160];
	snprintf(tmp, sizeof(tmp), "%s.tmp", PATH_SHADOW);

	/*
	 * Created 0600 before a byte is written. /etc/shadow is the one file
	 * rcd writes whose contents are the reason it is not world-readable,
	 * and rcd runs with no umask -- so the mode cannot be left to one.
	 */
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;

	if (!out) {
		RSS_WARN("passwd: cannot write %s: %s", tmp, strerror(errno));
		if (fd >= 0)
			close(fd);
		fclose(in);
		return -1;
	}

	/* Days since the epoch, which is what the third field counts. */
	long days = (long)(time(NULL) / 86400);
	size_t ulen = strlen(user);
	char line[512];
	bool found = false;
	bool ok = true;

	while (ok && fgets(line, sizeof(line), in)) {
		if (found || strncmp(line, user, ulen) != 0 || line[ulen] != ':') {
			ok = fputs(line, out) >= 0;
			continue;
		}
		found = true;

		/*
		 * name:passwd:lastchg:<the rest, untouched>. A line with fewer
		 * than three colons is not one this can edit safely, so it is
		 * carried through and the write fails below rather than
		 * producing a shadow file of a shape nothing else will parse.
		 */
		char *pw = line + ulen + 1;
		char *pw_end = strchr(pw, ':');
		char *age_end = pw_end ? strchr(pw_end + 1, ':') : NULL;

		/*
		 * A line longer than the buffer arrives in pieces, and the
		 * second piece would be written as a record of its own by the
		 * branch above -- a shadow file nothing else will parse. No
		 * real entry comes close to this, so it is refused rather than
		 * stitched back together.
		 */
		if (!strchr(line, '\n') && !feof(in)) {
			RSS_WARN("passwd: the %s line of %s is too long to edit", user,
				 PATH_SHADOW);
			found = false;
			ok = false;
			break;
		}

		if (!age_end) {
			RSS_WARN("passwd: the %s line of %s is malformed", user, PATH_SHADOW);
			found = false;
			ok = false;
			break;
		}
		ok = fprintf(out, "%.*s:%s:%ld:%s", (int)ulen, line, hash, days, age_end + 1) >= 0;
	}

	/*
	 * On flash, and durability is the whole point of the rename: /etc is
	 * the jffs2 upper layer, so without this the rename can reach the
	 * medium ahead of the bytes it names. A power cut in that window
	 * leaves a shadow file that is short or empty -- no root line at all,
	 * which reads as MISSING and authenticates nobody. Every other outcome
	 * this writer guards against is recoverable and that one is not.
	 *
	 * It also decides what `ok` means. The claim answers before this
	 * returns, and an owner told their camera is claimed has been told
	 * something that must still be true after the next power cut.
	 */
	if (fflush(out) != 0 || fsync(fileno(out)) != 0) {
		RSS_WARN("passwd: cannot commit %s: %s", tmp, strerror(errno));
		ok = false;
	}
	if (fclose(out) != 0)
		ok = false;
	fclose(in);

	if (!found && ok) {
		RSS_WARN("passwd: %s has no %s line", PATH_SHADOW, user);
		ok = false;
	}
	if (!ok) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, PATH_SHADOW) != 0) {
		RSS_WARN("passwd: cannot replace %s: %s", PATH_SHADOW, strerror(errno));
		unlink(tmp);
		return -1;
	}

	/* And the directory entry the rename created, for the same reason.
	 * Reported but not failed: the file is already on the medium by here,
	 * and the write did happen. */
	int dfd = open(RCD_SYSCONF_DIR, O_RDONLY);

	if (dfd < 0 || fsync(dfd) != 0)
		RSS_WARN("passwd: %s was written but its directory was not synced", PATH_SHADOW);
	if (dfd >= 0)
		close(dfd);
	return 0;
}

/*
 * The stored value, which is the hash and not the password.
 *
 * It is the hash rather than a "set"/"unset" token because `set` decides
 * whether anything changed by reading this back either side of the write, and
 * a token that is equal to itself would report every password change as a
 * change to nothing. It never reaches a client: a value of this type is
 * reported as `readable: false` before the provider is asked -- see
 * emit_value() -- which is the same treatment every credential in the table
 * already gets.
 */
static int root_password_get(char *out, size_t outsz)
{
	return rss_shadow_hash(PATH_SHADOW, RCD_PASSWD_USER, out, outsz) ? 0 : -1;
}

static int root_password_set(const char *value)
{
	/*
	 * Emptying this store is not an operation. Every other provider takes
	 * `set("")` to mean "put it back to unconfigured", and unconfigured
	 * here is a camera anyone on the network may take -- so there is no
	 * value that means it and `resettable` is false. Refused rather than
	 * ignored: a caller that got here asked for something this cannot do.
	 */
	if (!value || !value[0]) {
		RSS_WARN("passwd: the root password cannot be cleared over the network");
		return -1;
	}

	char hash[RCD_VAL_MAX];

	if (value[0] == '$') {
		/* Pre-derived by the client, so the plaintext never crossed
		 * the network. Stored as it came: the grammar has already
		 * checked its shape, and re-hashing a hash would be storing
		 * the wrong thing. */
		rss_strlcpy(hash, value, sizeof(hash));
	} else if (!hash_password(value, hash, sizeof(hash))) {
		return -1;
	}

	if (shadow_write(RCD_PASSWD_USER, hash) != 0)
		return -1;

	RSS_INFO("passwd: the %s password was set", RCD_PASSWD_USER);
	return 0;
}

const rcd_provider_t rcd_provider_root_password = {
	.get = root_password_get,
	.set = root_password_set,
	.resettable = false,
};

cJSON *rcd_cmd_claim(rcd_state_t *st, const cJSON *root)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "password");

	if (!cJSON_IsString(v) || !v->valuestring)
		return rcd_err(RCD_E_MALFORMED, "claim needs a 'password'");

	/*
	 * The second of the two refusals, and the one that owns the store.
	 * rhd has already declined to forward this unauthenticated on a camera
	 * that is claimed; this is rcd not taking rhd's word for it.
	 *
	 * The distinction between the three ways of not being claimable is
	 * worth making to whoever is reading the answer: "somebody already has
	 * this camera" and "this image does not allow it" send an operator to
	 * completely different places.
	 */
	switch (rss_shadow_state(PATH_SHADOW, RCD_PASSWD_USER)) {
	case RSS_SHADOW_UNSET:
		break;
	case RSS_SHADOW_SET:
		return rcd_err(RCD_E_CLAIMED,
			       "this camera has already been claimed; sign in to change its "
			       "password");
	case RSS_SHADOW_LOCKED:
		return rcd_err(RCD_E_CLAIMED,
			       "this camera's root account is locked, so it cannot be claimed "
			       "over the network");
	case RSS_SHADOW_MISSING:
		return rcd_err(RCD_E_IO, "this camera has no root account to claim");
	}

	/*
	 * Composed as an ordinary edit rather than writing the store here, so
	 * the value goes through the same grammar, the same log line and the
	 * same store as a password change made from the console. A claim that
	 * could accept what a `set` would refuse is a second configuration
	 * writer with one key in it.
	 */
	cJSON *req = cJSON_CreateObject();
	if (!req)
		return NULL;
	cJSON_AddStringToObject(req, "section", "device");
	cJSON_AddStringToObject(req, "key", "root_password");
	cJSON_AddItemToObject(req, "value", cJSON_Duplicate(v, true));

	cJSON *resp = rcd_cmd_set(st, req);
	cJSON_Delete(req);
	return resp;
}
