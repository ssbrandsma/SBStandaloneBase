/*
 * Custom firmware candidate preparation and local delivery for Standalone Base.
 * http_server.c supplies browser/internal authentication. A bounded forked job
 * reuses image_render.c's public HTTP transport and firmware_image.c's reader.
 * The state file informs sbbase's existing Comet firmware subscription. Neither
 * this module nor its worker writes firmware, changes UBI or mounts
 * filesystems.
 */
#include "firmware.h"
#include "firmware_image.h"
#include "image_render.h"
#include "auth.h"
#include "settings.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/socket.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <time.h>
#include <unistd.h>

#define FW_HEADERS                                                             \
	"Content-Type: application/json\r\nCache-Control: "                    \
	"no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: "     \
	"no-referrer\r\n"
#define FW_LEASE_MS 45000u
#define FW_HANDOFF_MS 600000u

typedef struct {
	FirmwareImage image;
	char error[256];
	unsigned bytes;
	int valid;
} FirmwareResult;

static struct {
	char url[2048], sha[65], installed[64], candidate[512], temporary[512];
	char source[8], url_sha[65], checksum[65];
	char config[512], state_file[512], error[256], status[32];
	FirmwareImage image;
	FirmwareResult result;
	unsigned board, bytes;
	unsigned worker_board;
	char worker_installed[64];
	int port, enabled, physical, valid, remote_lock, pipe;
	pid_t worker;
	size_t result_bytes;
	uint64_t lease, handoff, deadline, next_state, sequence;
	time_t last_check, last_validated;
} F;

/* One upload shares F.worker with URL jobs; no per-client image allocations. */
static struct {
	struct mg_connection *connection;
	int active, input, complete, old_valid, cancelled;
	char failure[256];
	unsigned received, expected, rate;
	uint64_t started, last_data, allowance_time, allowance;
	char sha[65], temporary[512], target[512], old_status[32];
} U;

static void upload_finish(int successful);

/** Compare an HTTP string without requiring a NUL terminator. */
static int equals(struct mg_str text, const char *literal)
{
	return text.len == strlen(literal) &&
	       !memcmp(text.buf, literal, text.len);
}

/** Get a bounded text file, rejecting partial/oversized reads. */
static int read_text(const char *path, char *out, size_t capacity)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return 0;
	size_t length = fread(out, 1, capacity - 1, file);
	int ok = !ferror(file) && fgetc(file) == EOF;
	fclose(file);
	out[length] = 0;
	return ok;
}

/** Atomically persist private text; never mark previous validation trusted. */
static int atomic_text(const char *path, const char *text)
{
	char temporary[540];
	snprintf(temporary, sizeof temporary, "%s.new", path);
	int fd = open(
		temporary, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0)
		return 0;
	size_t length = strlen(text), offset = 0;
	while (offset < length) {
		ssize_t count = write(fd, text + offset, length - offset);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			break;
		offset += (size_t)count;
	}
	int ok = offset == length && fsync(fd) == 0;
	if (close(fd))
		ok = 0;
	if (ok)
		ok = rename(temporary, path) == 0;
	if (!ok)
		unlink(temporary);
	return ok;
}

/** Copy a JSON string only when its entire value fits; free parser allocation.
 */
static int
json_text(struct mg_str json, const char *key, char *out, size_t size)
{
	char *value = mg_json_get_str(json, key);
	int ok = value && strlen(value) < size;
	if (ok)
		strcpy(out, value);
	free(value);
	return ok;
}

/** Require a trusted lowercase SHA-256 rather than an optional checksum. */
static int digest_valid(const char *digest)
{
	if (strlen(digest) != 64)
		return 0;
	for (unsigned i = 0; i < 64; i++)
		if (!isdigit((unsigned char)digest[i]) &&
		    !(digest[i] >= 'a' && digest[i] <= 'f'))
			return 0;
	return 1;
}

/** Native eligibility always requires a fresh authenticated device lease. */
static int permitted(void)
{
	int compared;
	return F.valid && !U.active && F.enabled && F.physical &&
	       mg_millis() < F.lease &&
	       firmware_version_compare(
		       F.image.version, F.installed, &compared) &&
	       compared > 0;
}

/** Publish a small private descriptor for sbbase; no caller controls its URL.
 */
static void publish_state(void)
{
	char text[512];
	if (permitted())
		snprintf(text,
			 sizeof text,
			 "%llu\nhttp://127.0.0.1:%d/firmware/%s/"
			 "custom.baby.bin\n%s\n%s\n",
			 (unsigned long long)F.lease,
			 F.port,
			 F.sha,
			 F.installed,
			 F.image.version);
	else
		snprintf(text, sizeof text, "0\n\n");
	atomic_text(F.state_file, text);
}

/** Save only descriptive metadata/configuration, never persistent permission.
 */
static int save_config(void)
{
	char text[4096];
	mg_snprintf(text,
		    sizeof text,
		    "{\"url\":%m,\"sha256\":%m,\"urlSha256\":%m,\"checksum\":%"
		    "m,\"source\":%m,"
		    "\"lastCheck\":%lld,\"lastValidated\":%lld,"
		    "\"lastCandidate\":%m,\"lastStatus\":%m}\n",
		    MG_ESC(F.url),
		    MG_ESC(F.sha),
		    MG_ESC(F.url_sha),
		    MG_ESC(F.checksum),
		    MG_ESC(F.source),
		    (long long)F.last_check,
		    (long long)F.last_validated,
		    MG_ESC(F.image.version),
		    MG_ESC(F.status));
	return atomic_text(F.config, text);
}

/** Select only the already active writable Extended Storage mount. */
static int stage_directory(char *out, size_t capacity)
{
	const char *root = "/mnt/sbdata";
#ifdef SB_FIRMWARE_TESTING
	if (getenv("SB_FIRMWARE_TEST_STAGE"))
		root = getenv("SB_FIRMWARE_TEST_STAGE");
	else
#endif
	{
		FILE *mounts = fopen("/proc/mounts", "r");
		char line[1024], device[128], mount[256], type[32];
		int found = 0;
		if (!mounts)
			return 0;
		while (fgets(line, sizeof line, mounts))
			if (sscanf(line,
				   "%127s %255s %31s",
				   device,
				   mount,
				   type) == 3 &&
			    !strcmp(mount, root) &&
			    (!strcmp(type, "sbubifs") ||
			     !strcmp(type, "ubifs")) &&
			    (!strcmp(device, "ubi0:sbdata") ||
			     !strcmp(device, "/dev/ubi0_5")))
				found = 1;
		fclose(mounts);
		if (!found)
			return 0;
	}
	if (access(root, W_OK) ||
	    snprintf(out, capacity, "%s/firmware", root) >= (int)capacity)
		return 0;
	if (mkdir(out, 0700) && errno != EEXIST)
		return 0;
	struct stat st;
	return !lstat(out, &st) && S_ISDIR(st.st_mode) && !(st.st_mode & 0022);
}

/** Read installed OS/CPU identity and reject incomplete or backup-boot layouts.
 */
static int identity_ok(const char *version, unsigned board)
{
#ifdef SB_FIRMWARE_TESTING
	if (getenv("SB_FIRMWARE_TEST_STAGE"))
		return board > 0;
#endif
	char os_version[512], cpu[4096], command[2048];
	if (!read_text(
		    "/etc/squeezeos.version", os_version, sizeof os_version) ||
	    !read_text("/proc/cpuinfo", cpu, sizeof cpu) ||
	    !read_text("/proc/cmdline", command, sizeof command))
		return 0;
	char *end = strpbrk(os_version, "\r\n");
	if (end)
		*end = 0;
	if (strcmp(os_version, version) || strstr(command, "cramfs_bak") ||
	    !strstr(command, "cramfs"))
		return 0;
	char *revision = strstr(cpu, "Revision"),
	     *hardware = strstr(cpu, "Hardware");
	if (!revision || !hardware || !strcasestr(hardware, "baby"))
		return 0;
	revision = strchr(revision, ':');
	return revision && strtoul(revision + 1, NULL, 10) == board;
}

/** Inspect UBI free/reclaimable native backup blocks without changing volumes.
 */
static int ubi_headroom(void)
{
#ifdef SB_FIRMWARE_TESTING
	if (getenv("SB_FIRMWARE_TEST_STAGE"))
		return 1;
#endif
	char text[128];
	if (!read_text(
		    "/sys/class/ubi/ubi0/eraseblock_size", text, sizeof text))
		return 0;
	unsigned long block = strtoul(text, NULL, 10);
	if (!block || !read_text("/sys/class/ubi/ubi0/avail_eraseblocks",
				 text,
				 sizeof text))
		return 0;
	unsigned long available = strtoul(text, NULL, 10);
	for (unsigned id = 0; id < 16; id++) {
		char path[128];
		snprintf(path, sizeof path, "/sys/class/ubi/ubi0_%u/name", id);
		if (!read_text(path, text, sizeof text))
			continue;
		text[strcspn(text, "\r\n")] = 0;
		if (!strcmp(text, "kernel_bak") ||
		    !strcmp(text, "cramfs_bak")) {
			snprintf(path,
				 sizeof path,
				 "/sys/class/ubi/ubi0_%u/reserved_ebs",
				 id);
			if (!read_text(path, text, sizeof text))
				return 0;
			available += strtoul(text, NULL, 10);
		}
	}
	unsigned long needed = (F.image.kernel_size + block - 1) / block +
			       (F.image.root_size + block - 1) / block + 8;
	return available >= needed;
}

/** Revoke an advertisement on any error, without touching a live file reader.
 */
static void fail(const char *message)
{
	F.valid = 0;
	snprintf(F.status, sizeof F.status, "blocked");
	snprintf(F.error, sizeof F.error, "%s", message);
	publish_state();
}

/** Initialize configuration and explicitly forget trusted/runtime state. */
void firmware_init(int port)
{
	memset(&F, 0, sizeof F);
	memset(&U, 0, sizeof U);
	U.input = -1;
	F.pipe = -1;
	F.port = port;
	const char *config = getenv("SBWEBSERVER_CONFIG_DIR");
	if (!config)
		config = "/mnt/storage/sbwebserver";
	const char *run = "/tmp/standalonebase";
#ifdef SB_FIRMWARE_TESTING
	if (getenv("SB_FIRMWARE_TEST_RUN"))
		run = getenv("SB_FIRMWARE_TEST_RUN");
#endif
	mkdir(run, 0700);
	snprintf(F.config, sizeof F.config, "%s/firmware.json", config);
	snprintf(F.state_file, sizeof F.state_file, "%s/firmware.state", run);
	char text[4096];
	if (read_text(F.config, text, sizeof text)) {
		struct mg_str json = mg_str(text);
		json_text(json, "$.url", F.url, sizeof F.url);
		json_text(json, "$.sha256", F.sha, sizeof F.sha);
		json_text(json, "$.urlSha256", F.url_sha, sizeof F.url_sha);
		json_text(json, "$.checksum", F.checksum, sizeof F.checksum);
		json_text(json, "$.source", F.source, sizeof F.source);
		if (!F.url_sha[0] && strcmp(F.source, "upload"))
			strcpy(F.url_sha, F.sha);
		if (!F.checksum[0])
			strcpy(F.checksum, F.sha);
		/* Old SHA-256 configurations remain usable without a migration
		 * write. */
		firmware_checksum_normalize(F.checksum, F.checksum);
		firmware_checksum_normalize(F.url_sha, F.url_sha);
		double checked;
		if (mg_json_get_num(json, "$.lastCheck", &checked))
			F.last_check = (time_t)checked;
		if (mg_json_get_num(json, "$.lastValidated", &checked))
			F.last_validated = (time_t)checked;
	}
	if (strcmp(F.source, "upload"))
		strcpy(F.source, "url");
	char stage[400], abandoned[512];
	if (stage_directory(stage, sizeof stage)) {
		snprintf(abandoned, sizeof abandoned, "%s/upload.tmp", stage);
		unlink(abandoned);
	}
	snprintf(F.status,
		 sizeof F.status,
		 "%s",
		 F.url[0] || F.sha[0] ? "unchecked" : "unconfigured");
	publish_state();
}

/** Start/revalidate one bounded worker; caller has already passed write auth.
 */
static int prepare(void)
{
	char directory[400];
	if (F.worker || F.handoff > mg_millis())
		return 409;
	if (!F.installed[0]) {
		fail("Physical Radio identity is not available");
		return 409;
	}
	char expected[65];
	if ((strcmp(F.source, "upload") && !sb_fetch_public_url_valid(F.url)) ||
	    !firmware_checksum_normalize(F.checksum, expected)) {
		fail("Configure a direct native .bin URL and expected SHA-1 or "
		     "SHA-256 "
		     "first");
		return 400;
	}
	if (!stage_directory(directory, sizeof directory)) {
		fail("Active writable Extended Storage is required for "
		     "firmware staging");
		return 507;
	}
	if (digest_valid(F.sha))
		snprintf(F.candidate,
			 sizeof F.candidate,
			 "%s/%s.bin",
			 directory,
			 F.sha);
	else
		F.candidate[0] = 0;
	snprintf(F.temporary, sizeof F.temporary, "%s/download.tmp", directory);
	/* This private directory holds only digest-named candidates. Never
	 * follow links or remove unrelated files. Existing open readers retain
	 * their fd. */
	DIR *entries = opendir(directory);
	if (!entries) {
		fail("Cannot inspect firmware staging directory");
		return 507;
	}
	struct dirent *entry;
	while ((entry = readdir(entries))) {
		if (strlen(entry->d_name) != 68 ||
		    strcmp(entry->d_name + 64, ".bin"))
			continue;
		char digest[65], path[512];
		memcpy(digest, entry->d_name, 64);
		digest[64] = 0;
		if (!digest_valid(digest) || !strcmp(digest, F.sha))
			continue;
		snprintf(path, sizeof path, "%s/%s.bin", directory, digest);
		struct stat old;
		if (!lstat(path, &old) && S_ISREG(old.st_mode) &&
		    unlink(path)) {
			closedir(entries);
			fail("Cannot remove obsolete firmware candidate");
			return 507;
		}
	}
	closedir(entries);
	struct statvfs space;
	if (statvfs(directory, &space)) {
		fail("Cannot determine firmware staging space");
		return 507;
	}
	/* Revalidation reads an existing file: no second image allocation.
	 * Unknown-length downloads reserve the entire enforced 24 MiB cap.
	 * The 4 MiB reserve covers filesystem metadata and concurrent activity.
	 */
	struct stat staged;
	int cached = !lstat(F.candidate, &staged) && S_ISREG(staged.st_mode) &&
		     staged.st_size > 0 && staged.st_size <= FW_IMAGE_LIMIT;
	if (!cached && !strcmp(F.source, "upload")) {
		fail("Staged uploaded image is missing; upload the file again");
		return 409;
	}
	uint64_t available = (uint64_t)space.f_bavail * space.f_frsize;
#ifdef SB_FIRMWARE_TESTING
	if (getenv("SB_FIRMWARE_TEST_FREE_BYTES"))
		available = strtoull(
			getenv("SB_FIRMWARE_TEST_FREE_BYTES"), NULL, 10);
#endif
	uint64_t required = 4u * 1024u * 1024u + (cached ? 0 : FW_IMAGE_LIMIT);
	if (available < required) {
		fail(cached ? "Insufficient staging reserve (4 MiB free "
			      "required for cached validation)"
			    : "Insufficient staging space (28 MiB free "
			      "required for an unknown-size download)");
		return 507;
	}
	int descriptors[2];
	if (pipe(descriptors)) {
		fail("Cannot create firmware worker channel");
		return 503;
	}
	F.valid = 0;
	F.bytes = 0;
	F.error[0] = 0;
	memset(&F.result, 0, sizeof F.result);
	F.result_bytes = 0;
	F.last_check = time(NULL);
	F.worker_board = F.board;
	strcpy(F.worker_installed, F.installed);
	F.worker = fork();
	if (F.worker == 0) {
		close(descriptors[0]);
		for (int fd = 3; fd < 1024; fd++)
			if (fd != descriptors[1])
				close(fd);
		FirmwareResult result = {0};
		char streamed_sha1[41], streamed_sha256[65];
		struct stat old;
		int cached = !lstat(F.candidate, &old) && S_ISREG(old.st_mode);
		if (cached || sb_fetch_firmware_file(F.url,
						     F.temporary,
						     &result.bytes,
						     streamed_sha1,
						     streamed_sha256) == 200) {
			if (cached)
				result.bytes = (unsigned)old.st_size;
			if (!cached &&
			    strcmp(expected,
				   strlen(expected) == 40 ? streamed_sha1
							  : streamed_sha256))
				snprintf(result.error,
					 sizeof result.error,
					 "Firmware checksum does not match "
					 "supplied digest");
			else
				result.valid = firmware_image_validate(
					cached ? F.candidate : F.temporary,
					expected,
					F.board,
					&result.image,
					result.error,
					sizeof result.error);
			if (result.valid && cached &&
			    strcmp(result.image.sha256, F.sha)) {
				result.valid = 0;
				snprintf(result.error,
					 sizeof result.error,
					 "Cached firmware SHA-256 identity "
					 "changed");
			}
			if (result.valid && !cached) {
				char target[512];
				snprintf(target,
					 sizeof target,
					 "%s/%s.bin",
					 directory,
					 result.image.sha256);
				int fd = open(F.temporary,
					      O_RDONLY | O_NOFOLLOW);
				if (fd < 0 || fsync(fd) ||
				    rename(F.temporary, target)) {
					result.valid = 0;
					snprintf(result.error,
						 sizeof result.error,
						 "Cannot publish staged image");
				}
				if (fd >= 0)
					close(fd);
			}
		} else
			snprintf(result.error,
				 sizeof result.error,
				 "Firmware download failed, timed out or "
				 "exceeded limit");
		unlink(F.temporary);
		ssize_t ignored = write(descriptors[1], &result, sizeof result);
		(void)ignored;
		close(descriptors[1]);
		_exit(0);
	}
	close(descriptors[1]);
	if (F.worker < 0) {
		close(descriptors[0]);
		F.worker = 0;
		fail("Cannot start firmware worker");
		return 503;
	}
	F.pipe = descriptors[0];
	fcntl(F.pipe, F_SETFL, O_NONBLOCK);
	F.deadline = mg_millis() + 360000;
	snprintf(F.status, sizeof F.status, "preparing");
	publish_state();
	save_config();
	return 202;
}

/** Poll worker results and permission expiry without blocking Mongoose. */
void firmware_tick(void)
{
	uint64_t now = mg_millis();
	if (F.worker) {
		ssize_t got = read(F.pipe,
				   (char *)&F.result + F.result_bytes,
				   sizeof F.result - F.result_bytes);
		if (got > 0)
			F.result_bytes += (size_t)got;
		int status;
		pid_t finished = waitpid(F.worker, &status, WNOHANG);
		if (now > F.deadline && !finished)
			kill(F.worker, SIGKILL);
		if (finished == F.worker) {
			while (F.result_bytes < sizeof F.result) {
				got = read(F.pipe,
					   (char *)&F.result + F.result_bytes,
					   sizeof F.result - F.result_bytes);
				if (got <= 0)
					break;
				F.result_bytes += (size_t)got;
			}
			close(F.pipe);
			F.pipe = -1;
			F.worker = 0;
			if (U.active) {
				upload_finish(F.result_bytes ==
						      sizeof F.result &&
					      WIFEXITED(status) &&
					      WEXITSTATUS(status) == 0 &&
					      F.result.valid);
				return;
			}
			if (F.result_bytes != sizeof F.result ||
			    !WIFEXITED(status) || !F.result.valid)
				fail(F.result.error[0]
					     ? F.result.error
					     : "Firmware worker failed");
			else {
				F.image = F.result.image;
				strcpy(F.sha, F.image.sha256);
				char directory[400];
				if (stage_directory(directory,
						    sizeof directory))
					snprintf(F.candidate,
						 sizeof F.candidate,
						 "%s/%s.bin",
						 directory,
						 F.sha);
				F.bytes = F.result.bytes;
				int compared;
				if (F.worker_board != F.board ||
				    strcmp(F.worker_installed, F.installed))
					fail("Radio identity changed during "
					     "firmware preparation");
				else if (!firmware_version_compare(
						 F.image.version,
						 F.installed,
						 &compared) ||
					 compared <= 0)
					fail("Candidate is not newer than "
					     "installed firmware");
				else if (!ubi_headroom())
					fail("Insufficient or unknown native "
					     "UBI update headroom");
				else {
					F.valid = 1;
					F.last_validated = time(NULL);
					snprintf(F.status,
						 sizeof F.status,
						 "ready");
				}
			}
			unlink(F.temporary);
			save_config();
			publish_state();
		}
	}
	if (now >= F.next_state) {
		F.next_state = now + 1000;
		publish_state();
	}
}

/** Keep Virtual Radio injection disabled until the device explicitly unlocks.
 */
int firmware_remote_locked(void)
{
	return F.remote_lock;
}

/** Serialize safe browser/device status; never include internal capabilities.
 */
static void status_reply(struct mg_connection *c)
{
	char hook[4096];
	int retained =
		access("/etc/standalonebase/storage-boot.sh", R_OK) == 0 &&
		read_text("/etc/init.d/rcS.local", hook, sizeof hook) &&
		strstr(hook, "/etc/standalonebase/storage-boot.sh") != NULL;
	struct stat st;
	unsigned bytes = F.bytes;
	if (U.active)
		bytes = U.received;
	else if (F.worker && !stat(F.temporary, &st))
		bytes = (unsigned)st.st_size;
	mg_http_reply(
		c,
		200,
		FW_HEADERS,
		"{\"url\":%m,\"sha256\":%m,\"installed\":%m,\"available\":%m,"
		"\"state\":%m,\"error\":%m,\"radioPermission\":%s,\"ready\":%s,"
		"\"received\":%u,\"lastCheck\":%lld,\"firmwareUrl\":%m,"
		"\"candidateReady\":%s,\"board\":%u,\"uploadSize\":%u,"
		"\"source\":%m,\"urlSha256\":%m,\"checksum\":%m,"
		"\"checksumAlgorithm\":%m,\"calculatedSha256\":%m,"
		"\"imageSize\":%u,"
		"\"urlChecksum\":%m,\"lastValidated\":%lld,"
		"\"bootstrapRetained\":%s}",
		MG_ESC(F.url),
		MG_ESC(F.sha),
		MG_ESC(F.installed),
		MG_ESC(F.image.version),
		MG_ESC(F.status),
		MG_ESC(F.error),
		F.enabled && mg_millis() < F.lease ? "true" : "false",
		permitted() ? "true" : "false",
		bytes,
		(long long)F.last_check,
		MG_ESC(""),
		F.valid ? "true" : "false",
		F.board,
		U.active ? U.expected : 0,
		MG_ESC(F.source),
		MG_ESC(F.url_sha),
		MG_ESC(F.checksum),
		MG_ESC(strlen(F.checksum) == 40	  ? "SHA-1"
		       : strlen(F.checksum) == 64 ? "SHA-256"
						  : ""),
		MG_ESC(F.image.sha256),
		F.image.size,
		MG_ESC(F.url_sha),
		(long long)F.last_validated,
		retained ? "true" : "false");
}

/** End one upload, preserving the old candidate unless a new one passes all
 * gates. */
static void upload_finish(int successful)
{
	int compared;
	FirmwareImage previous = F.image;
	if (U.cancelled)
		successful = 0;
	if (successful &&
	    (F.worker_board != F.board ||
	     strcmp(F.worker_installed, F.installed) ||
	     !firmware_version_compare(
		     F.result.image.version, F.installed, &compared) ||
	     compared <= 0)) {
		successful = 0;
		snprintf(U.failure,
			 sizeof U.failure,
			 "Uploaded image is not newer or Radio identity "
			 "changed");
	}
	if (successful) {
		F.image = F.result.image;
		if (!ubi_headroom()) {
			successful = 0;
			snprintf(U.failure,
				 sizeof U.failure,
				 "Insufficient or unknown native UBI update "
				 "headroom");
		}
		F.image = previous;
	}
	if (successful) {
		char directory[400];
		if (!stage_directory(directory, sizeof directory)) {
			successful = 0;
			snprintf(U.failure,
				 sizeof U.failure,
				 "Staging storage is unavailable");
		} else
			snprintf(U.target,
				 sizeof U.target,
				 "%s/%s.bin",
				 directory,
				 F.result.image.sha256);
	}
	if (successful && rename(U.temporary, U.target)) {
		successful = 0;
		snprintf(U.failure,
			 sizeof U.failure,
			 "Cannot publish validated upload");
	}
	if (U.input >= 0)
		close(U.input);
	U.input = -1;
	if (successful) {
		if (F.candidate[0] && strcmp(F.candidate, U.target))
			unlink(F.candidate);
		strcpy(F.candidate, U.target);
		strcpy(F.sha, F.result.image.sha256);
		strcpy(F.checksum, U.sha);
		strcpy(F.source, "upload");
		F.image = F.result.image;
		F.bytes = F.result.bytes;
		F.valid = 1;
		F.last_validated = time(NULL);
		F.error[0] = 0;
		snprintf(F.status, sizeof F.status, "ready");
	} else {
		F.valid = U.old_valid && F.worker_board == F.board &&
			  !strcmp(F.worker_installed, F.installed);
		snprintf(F.status,
			 sizeof F.status,
			 "%s",
			 F.valid ? U.old_status : "failed");
		snprintf(F.error,
			 sizeof F.error,
			 "%s",
			 U.failure[0]	     ? U.failure
			 : F.result.error[0] ? F.result.error
					     : "Firmware upload worker failed");
	}
	unlink(U.temporary);
	U.active = 0;
	F.last_check = time(NULL);
	save_config();
	publish_state();
	if (U.connection) {
		if (successful)
			status_reply(U.connection);
		else
			mg_http_reply(U.connection,
				      400,
				      FW_HEADERS,
				      "{\"error\":%m}",
				      MG_ESC(F.error));
		U.connection->is_draining = 1;
		U.connection = NULL;
	}
}

/** Cancel the sole upload without blocking or altering a previous candidate. */
static void upload_abort(const char *message, int code)
{
	if (!U.active || U.cancelled)
		return;
	U.cancelled = 1;
	snprintf(U.failure, sizeof U.failure, "%s", message);
	if (U.input >= 0)
		close(U.input);
	U.input = -1;
	if (F.worker)
		kill(F.worker, SIGKILL);
	if (U.connection) {
		mg_http_reply(U.connection,
			      code,
			      FW_HEADERS,
			      "{\"error\":%m}",
			      MG_ESC(message));
		U.connection->is_draining = 1;
		U.connection = NULL;
	}
}

/** Child-only bounded write, incremental SHA and shared image inspector. */
static void upload_worker(int input, int result_fd, pid_t parent)
{
#ifdef __linux__
	prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
	if (getppid() != parent)
		_exit(1);
	for (int fd = 3; fd < 1024; fd++)
		if (fd != input && fd != result_fd)
			close(fd);
	FirmwareResult result = {0};
	unsigned char buffer[32768], digest[32];
	char hex[65];
	mg_sha256_ctx sha;
	mg_sha1_ctx sha1;
	mg_sha256_init(&sha);
	mg_sha1_init(&sha1);
	int file = open(
		U.temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (file < 0)
		snprintf(result.error,
			 sizeof result.error,
			 "Cannot create private upload file");
	while (file >= 0 && result.bytes < U.expected) {
		size_t needed = U.expected - result.bytes;
		if (needed > sizeof buffer)
			needed = sizeof buffer;
		size_t filled = 0;
		while (filled < needed) {
			ssize_t n =
				read(input, buffer + filled, needed - filled);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			filled += (size_t)n;
		}
		ssize_t count = (ssize_t)filled;
		if (filled != needed) {
			snprintf(result.error,
				 sizeof result.error,
				 "Truncated upload");
			break;
		}
		struct statvfs space;
		if (statvfs(U.temporary, &space) ||
		    (uint64_t)space.f_bavail * space.f_frsize <
			    4u * 1024u * 1024u + (unsigned)count) {
			snprintf(result.error,
				 sizeof result.error,
				 "Upload exhausted staging safety reserve");
			break;
		}
		size_t written = 0;
		while (written < (size_t)count) {
			ssize_t n = write(file,
					  buffer + written,
					  (size_t)count - written);
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0)
				break;
			written += (size_t)n;
		}
		if (written != (size_t)count) {
			snprintf(result.error,
				 sizeof result.error,
				 "Cannot write complete upload");
			break;
		}
		mg_sha256_update(&sha, buffer, (size_t)count);
		mg_sha1_update(&sha1, buffer, (size_t)count);
		result.bytes += (unsigned)count;
	}
	if (file >= 0) {
		if (fsync(file) && !result.error[0])
			snprintf(result.error,
				 sizeof result.error,
				 "Cannot flush upload");
		if (close(file) && !result.error[0])
			snprintf(result.error,
				 sizeof result.error,
				 "Cannot close upload");
	}
	mg_sha256_final(digest, &sha);
	unsigned length = 32;
	unsigned char digest1[20];
	mg_sha1_final(digest1, &sha1);
	if (strlen(U.sha) == 40) {
		memcpy(digest, digest1, 20);
		length = 20;
	}
	for (unsigned i = 0; i < length; i++)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	if (!result.error[0] && strcmp(hex, U.sha))
		snprintf(result.error,
			 sizeof result.error,
			 "Firmware checksum does not match supplied digest");
	if (!result.error[0])
		result.valid = firmware_image_validate(U.temporary,
						       U.sha,
						       F.board,
						       &result.image,
						       result.error,
						       sizeof result.error);
	if (!result.valid)
		unlink(U.temporary);
	(void)!write(result_fd, &result, sizeof result);
	close(input);
	close(result_fd);
	_exit(0);
}

/** Move at most one bounded batch into the worker, propagating backpressure. */
static void upload_pump(struct mg_connection *c)
{
	uint64_t now = mg_millis();
	unsigned idle = 30000;
#ifdef SB_FIRMWARE_TESTING
	if (getenv("SB_FIRMWARE_TEST_UPLOAD_IDLE_MS"))
		idle = (unsigned)strtoul(
			getenv("SB_FIRMWARE_TEST_UPLOAD_IDLE_MS"), NULL, 10);
#endif
	if (now - U.started > 600000 ||
	    (!U.complete && now - U.last_data > idle)) {
		upload_abort("Firmware upload timed out", 408);
		return;
	}
	if (U.complete || U.cancelled)
		return;
	if (c->recv.len > U.expected - U.received) {
		upload_abort("Upload body exceeds declared length", 400);
		return;
	}
	if (U.rate) {
		U.allowance += (now - U.allowance_time) * U.rate;
		if (U.allowance > 32768u * 1000u)
			U.allowance = 32768u * 1000u;
		U.allowance_time = now;
	}
	size_t count = c->recv.len < 32768 ? c->recv.len : 32768;
	if (U.rate && count > U.allowance / 1000)
		count = (size_t)(U.allowance / 1000);
	if (count) {
		ssize_t sent = send(U.input, c->recv.buf, count, MSG_NOSIGNAL);
		if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR) {
			upload_abort("Firmware upload worker stopped", 500);
			return;
		}
		if (sent > 0) {
			mg_iobuf_del(&c->recv, 0, (size_t)sent);
			U.received += (unsigned)sent;
			U.last_data = now;
			if (U.rate)
				U.allowance -= (size_t)sent * 1000;
		}
	}
	c->is_full = c->recv.len >= 32768;
	if (U.received == U.expected) {
		U.complete = 1;
		shutdown(U.input, SHUT_WR);
		close(U.input);
		U.input = -1;
		c->is_full = 1;
		snprintf(F.status, sizeof F.status, "validating");
	}
}

/** Own raw upload framing before Mongoose buffers the body; no filename paths.
 */
int firmware_upload_event(struct mg_connection *c,
			  int event,
			  void *data,
			  int readonly)
{
	if (U.active && c == U.connection) {
		if (event == MG_EV_CLOSE) {
			U.connection = NULL;
			upload_abort(
				"Firmware upload cancelled or disconnected",
				400);
		} else if (event == MG_EV_READ || event == MG_EV_POLL)
			upload_pump(c);
		return 1;
	}
	if (event != MG_EV_HTTP_HDRS)
		return 0;
	struct mg_http_message *m = data;
	if (!equals(m->uri, "/api/firmware/upload") ||
	    !equals(m->method, "POST"))
		return 0;
	int code = 400;
	const char *error = "Invalid binary upload headers";
	struct mg_str *length = mg_http_get_header(m, "Content-Length");
	struct mg_str *type = mg_http_get_header(m, "Content-Type");
	struct mg_str *sha = mg_http_get_header(m, "X-Firmware-Checksum");
	struct mg_str *legacy_sha = mg_http_get_header(m, "X-Firmware-SHA256");
	char digest[65] = {0}, directory[400];
	uint64_t size = 0;
	if (!auth_session_ok(m, NULL)) {
		code = 401;
		error = "Browser pairing required";
		goto reject;
	}
	if (readonly || !auth_origin_ok(m)) {
		code = 403;
		error = "Write authorization denied";
		goto reject;
	}
	if (F.worker || U.active || F.handoff > mg_millis()) {
		code = 409;
		error = "Firmware preparation or handoff is active";
		goto reject;
	}
	if (!F.installed[0]) {
		code = 409;
		error = "Physical Radio identity is unavailable";
		goto reject;
	}
	if (!length) {
		code = 411;
		error = "Content-Length is required for firmware upload";
		goto reject;
	}
	if (mg_http_get_header(m, "Transfer-Encoding") || !type ||
	    !equals(*type, "application/octet-stream") || (sha && legacy_sha))
		goto reject;
	if (!sha)
		sha = legacy_sha;
	if (!sha || (sha->len != 40 && sha->len != 64) ||
	    (legacy_sha && sha->len != 64))
		goto reject;
	memcpy(digest, sha->buf, sha->len);
	if (strlen(digest) != sha->len ||
	    !firmware_checksum_normalize(digest, digest)) {
		error = "Expected checksum must be 40 (SHA-1) or 64 (SHA-256) "
			"hex characters";
		goto reject;
	}
	if (!length->len || length->len > 8) {
		code = 413;
		error = "Firmware exceeds 24 MiB limit";
		goto reject;
	}
	for (size_t i = 0; i < length->len; i++) {
		if (length->buf[i] < '0' || length->buf[i] > '9')
			goto reject;
		size = size * 10 + length->buf[i] - '0';
	}
	if (!size || size > FW_IMAGE_LIMIT) {
		code = 413;
		error = "Firmware must be between 1 byte and 24 MiB";
		goto reject;
	}
	unsigned lengths = 0, checksums = 0;
	for (unsigned i = 0; i < MG_MAX_HTTP_HEADERS && m->headers[i].name.len;
	     i++) {
		if (!mg_strcasecmp(m->headers[i].name,
				   mg_str("X-Firmware-Checksum")) ||
		    !mg_strcasecmp(m->headers[i].name,
				   mg_str("X-Firmware-SHA256")))
			checksums++;
		if (!mg_strcasecmp(m->headers[i].name,
				   mg_str("Content-Length")))
			lengths++;
	}
	if (lengths != 1 || checksums != 1)
		goto reject;
	if (!stage_directory(directory, sizeof directory)) {
		code = 507;
		error = "Active writable Extended Storage is required";
		goto reject;
	}
	struct statvfs space;
	if (statvfs(directory, &space)) {
		code = 507;
		error = "Cannot inspect staging space";
		goto reject;
	}
	uint64_t available = (uint64_t)space.f_bavail * space.f_frsize;
#ifdef SB_FIRMWARE_TESTING
	if (getenv("SB_FIRMWARE_TEST_FREE_BYTES"))
		available = strtoull(
			getenv("SB_FIRMWARE_TEST_FREE_BYTES"), NULL, 10);
#endif
	if (available < size + 4u * 1024u * 1024u) {
		code = 507;
		error = "Insufficient space for upload plus 4 MiB reserve";
		goto reject;
	}
	int input[2], output[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, input)) {
		code = 503;
		error = "Cannot create upload input channel";
		goto reject;
	}
	if (pipe(output)) {
		close(input[0]);
		close(input[1]);
		code = 503;
		error = "Cannot create upload result channel";
		goto reject;
	}
	memset(&U, 0, sizeof U);
	U.active = 1;
	U.input = input[0];
	U.connection = c;
	U.expected = (unsigned)size;
	U.old_valid = F.valid;
	strcpy(U.old_status, F.status);
	strcpy(U.sha, digest);
	snprintf(U.temporary, sizeof U.temporary, "%s/upload.tmp", directory);
	U.started = U.last_data = U.allowance_time = mg_millis();
	const char *rate = getenv("SB_FIRMWARE_UPLOAD_RATE_KIB");
	if (rate) {
		unsigned long n = strtoul(rate, NULL, 10);
		if (n >= 8 && n <= 1024)
			U.rate = (unsigned)n * 1024;
	}
	F.worker_board = F.board;
	strcpy(F.worker_installed, F.installed);
	memset(&F.result, 0, sizeof F.result);
	F.result_bytes = 0;
	pid_t parent = getpid();
	F.worker = fork();
	if (!F.worker) {
		close(input[0]);
		close(output[0]);
		upload_worker(input[1], output[1], parent);
	}
	close(input[1]);
	close(output[1]);
	if (F.worker < 0) {
		close(input[0]);
		close(output[0]);
		U.active = 0;
		U.connection = NULL;
		F.worker = 0;
		code = 503;
		error = "Cannot start firmware upload worker";
		goto reject;
	}
	fcntl(U.input, F_SETFL, O_NONBLOCK);
	F.pipe = output[0];
	fcntl(F.pipe, F_SETFL, O_NONBLOCK);
	F.deadline = U.started + 660000;
	snprintf(F.status, sizeof F.status, "uploading");
	F.error[0] = 0;
	mg_iobuf_del(&c->recv, 0, m->head.len);
	c->pfn = NULL;
	publish_state();
	upload_pump(c);
	return 1;
reject:
	mg_http_reply(c, code, FW_HEADERS, "{\"error\":%m}", MG_ESC(error));
	c->is_draining = 1;
	c->is_full = 1;
	return 1;
}

/** Handle internal permission, browser configuration, and the exact local
 * image. internal is supplied by the existing capability+loopback
 * authenticator; browser calls reach here only after shared session validation.
 */
int firmware_handle(struct mg_connection *c,
		    struct mg_http_message *m,
		    int internal,
		    int readonly)
{
	int local_api = m->uri.len >= 23 &&
			!memcmp(m->uri.buf, "/api/internal/firmware/", 23);
	int browser_api =
		m->uri.len >= 14 && !memcmp(m->uri.buf, "/api/firmware/", 14);
	int download =
		m->uri.len >= 10 && !memcmp(m->uri.buf, "/firmware/", 10);
	if (!local_api && !browser_api && !download)
		return 0;
	if (m->body.len > 4096) {
		mg_http_reply(c,
			      413,
			      FW_HEADERS,
			      "{\"error\":\"Request too large\"}");
		return 1;
	}
	if (local_api) {
		if (!internal || mg_http_get_header(m, "Origin")) {
			mg_http_reply(c,
				      403,
				      FW_HEADERS,
				      "{\"error\":\"Device authorization "
				      "required\"}");
			return 1;
		}
		if (equals(m->uri, "/api/internal/firmware/permission") &&
		    equals(m->method, "POST")) {
			char installed[64], machine[16];
			double board, sequence;
			bool enabled, physical;
			int compared;
			if (!json_text(m->body,
				       "$.installed",
				       installed,
				       sizeof installed) ||
			    !json_text(m->body,
				       "$.machine",
				       machine,
				       sizeof machine) ||
			    strcmp(machine, "baby") ||
			    !mg_json_get_num(m->body, "$.board", &board) ||
			    board < 1 || board > 1000000 ||
			    board != (unsigned)board ||
			    !mg_json_get_bool(m->body, "$.enabled", &enabled) ||
			    !mg_json_get_bool(m->body,
					      "$.physicalSupported",
					      &physical) ||
			    !mg_json_get_num(
				    m->body, "$.sequence", &sequence) ||
			    sequence < 1 || sequence > 9007199254740991.0 ||
			    sequence != (uint64_t)sequence ||
			    !firmware_version_compare(
				    installed, installed, &compared) ||
			    !identity_ok(installed, (unsigned)board)) {
				F.enabled = 0;
				F.lease = 0;
				publish_state();
				mg_http_reply(c,
					      400,
					      FW_HEADERS,
					      "{\"error\":\"Device identity "
					      "unavailable or inconsistent\"}");
				return 1;
			}
			if ((uint64_t)sequence <= F.sequence) {
				mg_http_reply(c,
					      409,
					      FW_HEADERS,
					      "{\"error\":\"Stale device "
					      "permission request\"}");
				return 1;
			}
			F.sequence = (uint64_t)sequence;
			if (strcmp(F.installed, installed) ||
			    F.board != (unsigned)board)
				F.valid = 0;
			strcpy(F.installed, installed);
			F.board = (unsigned)board;
			F.enabled = enabled;
			F.physical = physical;
			F.lease = mg_millis() + FW_LEASE_MS;
			F.remote_lock = enabled || F.handoff > mg_millis();
			if (!enabled)
				F.handoff = 0;
			publish_state();
			status_reply(c);
			return 1;
		}
		if (equals(m->uri, "/api/internal/firmware/review") &&
		    equals(m->method, "POST")) {
			if (!permitted() || !ubi_headroom()) {
				mg_http_reply(c,
					      409,
					      FW_HEADERS,
					      "{\"error\":\"No authorized "
					      "compatible candidate\"}");
				return 1;
			}
			F.handoff = mg_millis() + FW_HANDOFF_MS;
			F.remote_lock = 1;
			mg_http_reply(
				c,
				200,
				FW_HEADERS,
				"{\"firmwareUrl\":\"http://127.0.0.1:%d/"
				"firmware/%s/"
				"custom.baby.bin\",\"firmwareUpgrade\":0}",
				F.port,
				F.sha);
			return 1;
		}
		mg_http_reply(c, 404, FW_HEADERS, "{}");
		return 1;
	}
	if (download) {
		char expected[128];
		char host[64];
		snprintf(host, sizeof host, "127.0.0.1:%d", F.port);
		struct mg_str *requested_host = mg_http_get_header(m, "Host");
		snprintf(expected,
			 sizeof expected,
			 "/firmware/%s/custom.baby.bin",
			 F.sha);
		static const unsigned char loopback[4] = {127, 0, 0, 1};
		if (!equals(m->uri, expected) || c->rem.is_ip6 ||
		    memcmp(c->rem.ip, loopback, 4) ||
		    !equals(m->method, "GET") || !permitted() ||
		    !requested_host ||
		    (!equals(*requested_host, host) &&
		     !(F.port == 80 && equals(*requested_host, "127.0.0.1"))) ||
		    mg_http_get_header(m, "Sec-Fetch-Site") ||
		    mg_http_get_header(m, "Origin") ||
		    mg_http_get_header(m, "Range")) {
			mg_http_reply(c,
				      403,
				      FW_HEADERS,
				      "{\"error\":\"No active local firmware "
				      "handoff\"}");
			return 1;
		}
		struct mg_http_serve_opts options = {0};
		options.extra_headers =
			"Content-Type: "
			"application/octet-stream\r\nCache-Control: "
			"no-store\r\nX-Content-Type-Options: nosniff\r\n";
		mg_http_serve_file(c, m, F.candidate, &options);
		return 1;
	}
	if (equals(m->method, "GET")) {
		status_reply(c);
		return 1;
	}
	if (readonly || !auth_origin_ok(m)) {
		mg_http_reply(c,
			      403,
			      FW_HEADERS,
			      "{\"error\":\"Write authorization denied\"}");
		return 1;
	}
	if (equals(m->uri, "/api/firmware/config") &&
	    equals(m->method, "PUT")) {
		char url[2048], sha[65];
		if (F.worker || F.handoff > mg_millis()) {
			mg_http_reply(c,
				      409,
				      FW_HEADERS,
				      "{\"error\":\"Firmware "
				      "preparation/handoff is active\"}");
			return 1;
		}
		int has_checksum =
			mg_json_get(m->body, "$.checksum", NULL) >= 0;
		if (!json_text(m->body, "$.url", url, sizeof url) ||
		    !json_text(m->body,
			       has_checksum ? "$.checksum" : "$.sha256",
			       sha,
			       sizeof sha) ||
		    !sb_fetch_public_url_valid(url) ||
		    !firmware_checksum_normalize(sha, sha)) {
			mg_http_reply(c,
				      400,
				      FW_HEADERS,
				      "{\"error\":\"A valid direct HTTP(S) URL "
				      "and expected SHA-1 or SHA-256 are "
				      "required\"}");
			return 1;
		}
		int same_image =
			!strcmp(F.sha, sha) ||
			(F.image.sha1[0] && !strcmp(F.image.sha1, sha)) ||
			(!strcmp(F.checksum, sha) && !strcmp(F.url, url));
		if (same_image && !strcmp(F.source, "url") &&
		    !strcmp(F.url, url) && !strcmp(F.checksum, sha)) {
			status_reply(c);
			return 1;
		}
		if (F.candidate[0] && !same_image) {
			unlink(F.candidate);
			F.candidate[0] = 0;
		}
		strcpy(F.url, url);
		if (!same_image) {
			F.sha[0] = 0;
			memset(&F.image, 0, sizeof F.image);
		}
		if (strlen(sha) == 64)
			strcpy(F.sha, sha);
		strcpy(F.checksum, sha);
		strcpy(F.url_sha, sha);
		strcpy(F.source, "url");
		F.valid = 0;
		F.image.version[0] = 0;
		snprintf(F.status, sizeof F.status, "unchecked");
		publish_state();
		if (!save_config()) {
			mg_http_reply(c,
				      500,
				      FW_HEADERS,
				      "{\"error\":\"Cannot persist firmware "
				      "configuration\"}");
			return 1;
		}
		status_reply(c);
		return 1;
	}
	if (equals(m->uri, "/api/firmware/check") &&
	    equals(m->method, "POST")) {
		if (F.worker || F.handoff > mg_millis()) {
			mg_http_reply(c,
				      409,
				      FW_HEADERS,
				      "{\"error\":\"Firmware "
				      "preparation/handoff is active\"}");
			return 1;
		}
		if (mg_json_get(m->body, "$.checksum", NULL) >= 0) {
			char checksum[65];
			if (!json_text(m->body,
				       "$.checksum",
				       checksum,
				       sizeof checksum) ||
			    !firmware_checksum_normalize(checksum, checksum)) {
				mg_http_reply(c,
					      400,
					      FW_HEADERS,
					      "{\"error\":\"Expected SHA-1 or "
					      "SHA-256 hex checksum\"}");
				return 1;
			}
			strcpy(F.checksum, checksum);
			if (!strcmp(F.source, "url"))
				strcpy(F.url_sha, checksum);
		}
		int code = prepare();
		mg_http_reply(c,
			      code,
			      FW_HEADERS,
			      "{\"state\":%m,\"error\":%m}",
			      MG_ESC(F.status),
			      MG_ESC(F.error));
		return 1;
	}
	mg_http_reply(c,
		      405,
		      FW_HEADERS,
		      "{\"error\":\"Unsupported firmware operation\"}");
	return 1;
}
