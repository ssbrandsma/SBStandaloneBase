/* Isolated metadata-wait lifecycle/expiry/CPU tests. Include the registry to
 * move finished timestamps without adding a production test-clock API. */
#include "../../native/sbproxy/hls_metadata.c"
#include <assert.h>

typedef struct {
	int fd;
	char path[128];
	double cpu;
} request_state;

/** Measure this request thread only, not concurrent HLS/audio activity. */
static double thread_cpu(void)
{
	struct timespec now;
	assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now));
	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/** Serve one already-parsed request over a local socketpair. */
static void *serve(void *opaque)
{
	request_state *request = opaque;
	double start = thread_cpu();
	assert(hls_metadata_http(request->fd, request->path, 0) == 1);
	request->cpu = thread_cpu() - start;
	close(request->fd);
	return NULL;
}

/** Verify the idle timeout sleeps and finished-state expiry needs no thread. */
int main(void)
{
	char id[33];
	void *session = hls_metadata_open(id);
	assert(session);
	int sockets[2];
	assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
	request_state request = {0};
	request.fd = sockets[0];
	snprintf(request.path,
		 sizeof request.path,
		 "/hls-meta/%s?after=0&wait=1",
		 id);
	pthread_t thread;
	assert(!pthread_create(&thread, NULL, serve, &request));
	char body[1024];
	size_t used = 0;
	ssize_t n;
	while ((n = read(sockets[1], body + used, sizeof body - used - 1)) > 0)
		used += (size_t)n;
	body[used] = 0;
	close(sockets[1]);
	assert(!pthread_join(thread, NULL));
	assert(strstr(body, "200 Metadata") &&
	       strstr(body, "\"pending\":true"));
	assert(request.cpu <
	       .1); /* poll must not spin for its one-second wait */
	printf("metadata-wait-one-second-thread-cpu=%.6f seconds\n",
	       request.cpu);
	hls_metadata_finish(session);
	pthread_mutex_lock(&lock);
	((metadata_session *)session)->finished = monotonic_seconds() - 121;
	pthread_mutex_unlock(&lock);
	assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
	assert(hls_metadata_http(sockets[0], request.path, 0));
	close(sockets[0]);
	n = read(sockets[1], body, sizeof body - 1);
	assert(n > 0);
	body[n] = 0;
	assert(strstr(body, "404 Metadata"));
	close(sockets[1]);
	assert(!sessions[0] && !sessions[1] && !waiters[0] && !waiters[1]);
	printf("hls-wait-tests-ok waiter=%zu\n", sizeof(metadata_waiter));
	return 0;
}
