
#include <signal.h>
#include <bpf/libbpf.h>
#include <sys/resource.h>
#include "execsnoop_kern_skel.h"
#include "execsnoop_share.h"

namespace CGPROXY::EXECSNOOP {

#define PERF_BUFFER_PAGES 64
#define TASK_COMM_LEN 16
struct event {
	char comm[TASK_COMM_LEN];
	pid_t pid;
	pid_t tgid;
	pid_t ppid;
	uid_t uid;
};

function<int(int)> callback = NULL;
promise<void> status;

static void handle_event(void *ctx, int cpu, void *data, __u32 size) {
  	auto e = static_cast<event*>(data);
  	if (callback) callback(e->pid);
}

void handle_lost_events(void *ctx, int cpu, __u64 lost_cnt) {
	fprintf(stderr, "Lost %llu events on CPU #%d!\n", lost_cnt, cpu);
}

int bump_memlock_rlimit(void) {
	struct rlimit rlim_new = { RLIM_INFINITY, RLIM_INFINITY };
	return setrlimit(RLIMIT_MEMLOCK, &rlim_new);
}

int execsnoop() {
	struct perf_buffer_opts pb_opts = {};
	struct perf_buffer *pb;
	int err;
	
	err = bump_memlock_rlimit();
	if (err) {
		fprintf(stderr, "failed to increase rlimit: %d\n", err);
		return 1;
	}
	
	struct execsnoop_kern *obj=execsnoop_kern__open_and_load();
	if (!obj) {
		fprintf(stderr, "failed to open and/or load BPF object\n");
		return 1;
	}

	err = execsnoop_kern__attach(obj);
	if (err) {
		fprintf(stderr, "failed to attach BPF programs\n");
		return err;
	}

	pb_opts.sample_cb = handle_event;
	pb_opts.lost_cb = handle_lost_events;
	pb = perf_buffer__new(bpf_map__fd(obj->maps.perf_events), PERF_BUFFER_PAGES, &pb_opts);
	err = libbpf_get_error(pb);
	if (err) {
		printf("failed to setup perf_buffer: %d\n", err);
		return 1;
	}

	// notify
  	status.set_value();

	while ((err = perf_buffer__poll(pb, -1)) >= 0) {}
	kill(0, SIGINT);
	return err;
}

void startThread(function<int(int)> c, promise<void> _status) {
  status = move(_status);
  callback = c;
  execsnoop();
}

}
