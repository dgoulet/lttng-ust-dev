#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sched.h>
#include <fcntl.h>
#include <poll.h>

#include "marker.h"
#include "tracer.h"
#include "localerr.h"
#include "ustcomm.h"
#include "relay.h" /* FIXME: remove */
#include "marker-control.h"

//#define USE_CLONE

#define USTSIGNAL SIGIO

#define MAX_MSG_SIZE (100)
#define MSG_NOTIF 1
#define MSG_REGISTER_NOTIF 2

char consumer_stack[10000];

struct list_head blocked_consumers = LIST_HEAD_INIT(blocked_consumers);

static struct ustcomm_app ustcomm_app;

struct tracecmd { /* no padding */
	uint32_t size;
	uint16_t command;
};

//struct listener_arg {
//	int pipe_fd;
//};

struct trctl_msg {
	/* size: the size of all the fields except size itself */
	uint32_t size;
	uint16_t type;
	/* Only the necessary part of the payload is transferred. It
         * may even be none of it.
         */
	char payload[94];
};

struct consumer_channel {
	int fd;
	struct ltt_channel_struct *chan;
};

struct blocked_consumer {
	int fd_consumer;
	int fd_producer;
	int tmp_poll_idx;

	/* args to ustcomm_send_reply */
	struct ustcomm_server server;
	struct ustcomm_source src;

	/* args to ltt_do_get_subbuf */
	struct rchan_buf *rbuf;
	struct ltt_channel_buf_struct *lttbuf;

	struct list_head list;
};

static void print_markers(FILE *fp)
{
	struct marker_iter iter;

	lock_markers();
	marker_iter_reset(&iter);
	marker_iter_start(&iter);

	while(iter.marker) {
		fprintf(fp, "marker: %s_%s %d \"%s\"\n", iter.marker->channel, iter.marker->name, (int)imv_read(iter.marker->state), iter.marker->format);
		marker_iter_next(&iter);
	}
	unlock_markers();
}

void do_command(struct tracecmd *cmd)
{
}

void receive_commands()
{
}

int fd_notif = -1;
void notif_cb(void)
{
	int result;
	struct trctl_msg msg;

	/* FIXME: fd_notif should probably be protected by a spinlock */

	if(fd_notif == -1)
		return;

	msg.type = MSG_NOTIF;
	msg.size = sizeof(msg.type);

	/* FIXME: don't block here */
	result = write(fd_notif, &msg, msg.size+sizeof(msg.size));
	if(result == -1) {
		PERROR("write");
		return;
	}
}

static void inform_consumer_daemon(void)
{
	ustcomm_request_consumer(getpid(), "metadata");
	ustcomm_request_consumer(getpid(), "ust");
}

void process_blocked_consumers(void)
{
	int n_fds = 0;
	struct pollfd *fds;
	struct blocked_consumer *bc;
	int idx = 0;
	char inbuf;
	int result;

	list_for_each_entry(bc, &blocked_consumers, list) {
		n_fds++;
	}

	fds = (struct pollfd *) malloc(n_fds * sizeof(struct pollfd));
	if(fds == NULL) {
		ERR("malloc returned NULL");
		return;
	}

	list_for_each_entry(bc, &blocked_consumers, list) {
		fds[idx].fd = bc->fd_producer;
		fds[idx].events = POLLIN;
		bc->tmp_poll_idx = idx;
		idx++;
	}

	result = poll(fds, n_fds, 0);
	if(result == -1) {
		PERROR("poll");
		return;
	}

	list_for_each_entry(bc, &blocked_consumers, list) {
		if(fds[bc->tmp_poll_idx].revents) {
			long consumed_old = 0;
			char *reply;

			result = read(bc->fd_producer, &inbuf, 1);
			if(result == -1) {
				PERROR("read");
				continue;
			}
			if(result == 0) {
				DBG("PRODUCER END");

				close(bc->fd_producer);

				list_del(&bc->list);

				result = ustcomm_send_reply(&bc->server, "END", &bc->src);
				if(result < 0) {
					ERR("ustcomm_send_reply failed");
					continue;
				}

				continue;
			}

			result = ltt_do_get_subbuf(bc->rbuf, bc->lttbuf, &consumed_old);
			if(result == -EAGAIN) {
				WARN("missed buffer?");
				continue;
			}
			else if(result < 0) {
				DBG("ltt_do_get_subbuf: error: %s", strerror(-result));
			}
			asprintf(&reply, "%s %ld", "OK", consumed_old);
			result = ustcomm_send_reply(&bc->server, reply, &bc->src);
			if(result < 0) {
				ERR("ustcomm_send_reply failed");
				free(reply);
				continue;
			}
			free(reply);

			list_del(&bc->list);
		}
	}

}

void *listener_main(void *p)
{
	int result;

	DBG("LISTENER");

	for(;;) {
		char trace_name[] = "auto";
		char trace_type[] = "ustrelay";
		char *recvbuf;
		int len;
		struct ustcomm_source src;

		process_blocked_consumers();

		result = ustcomm_app_recv_message(&ustcomm_app, &recvbuf, &src, 5);
		if(result < 0) {
			WARN("error in ustcomm_app_recv_message");
			continue;
		}
		else if(result == 0) {
			/* no message */
			continue;
		}

		DBG("received a message! it's: %s\n", recvbuf);
		len = strlen(recvbuf);

		if(!strcmp(recvbuf, "print_markers")) {
			print_markers(stderr);
		}
		else if(!strcmp(recvbuf, "list_markers")) {
			char *ptr;
			size_t size;
			FILE *fp;

			fp = open_memstream(&ptr, &size);
			print_markers(fp);
			fclose(fp);

			result = ustcomm_send_reply(&ustcomm_app.server, ptr, &src);

			free(ptr);
		}
		else if(!strcmp(recvbuf, "start")) {
			/* start is an operation that setups the trace, allocates it and starts it */
			result = ltt_trace_setup(trace_name);
			if(result < 0) {
				ERR("ltt_trace_setup failed");
				return (void *)1;
			}

			result = ltt_trace_set_type(trace_name, trace_type);
			if(result < 0) {
				ERR("ltt_trace_set_type failed");
				return (void *)1;
			}

			result = ltt_trace_alloc(trace_name);
			if(result < 0) {
				ERR("ltt_trace_alloc failed");
				return (void *)1;
			}

			inform_consumer_daemon();

			result = ltt_trace_start(trace_name);
			if(result < 0) {
				ERR("ltt_trace_start failed");
				continue;
			}
		}
		else if(!strcmp(recvbuf, "trace_setup")) {
			DBG("trace setup");

			result = ltt_trace_setup(trace_name);
			if(result < 0) {
				ERR("ltt_trace_setup failed");
				return (void *)1;
			}

			result = ltt_trace_set_type(trace_name, trace_type);
			if(result < 0) {
				ERR("ltt_trace_set_type failed");
				return (void *)1;
			}
		}
		else if(!strcmp(recvbuf, "trace_alloc")) {
			DBG("trace alloc");

			result = ltt_trace_alloc(trace_name);
			if(result < 0) {
				ERR("ltt_trace_alloc failed");
				return (void *)1;
			}
		}
		else if(!strcmp(recvbuf, "trace_start")) {
			DBG("trace start");

			result = ltt_trace_start(trace_name);
			if(result < 0) {
				ERR("ltt_trace_start failed");
				continue;
			}
		}
		else if(!strcmp(recvbuf, "trace_stop")) {
			DBG("trace stop");

			result = ltt_trace_stop(trace_name);
			if(result < 0) {
				ERR("ltt_trace_stop failed");
				return (void *)1;
			}
		}
		else if(!strcmp(recvbuf, "trace_destroy")) {

			DBG("trace destroy");

			result = ltt_trace_destroy(trace_name);
			if(result < 0) {
				ERR("ltt_trace_destroy failed");
				return (void *)1;
			}
		}
		else if(nth_token_is(recvbuf, "get_shmid", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_shmid");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_shmid: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				ERR("cannot find trace!");
				return (void *)1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;
				struct rchan_buf *rbuf = rchan->buf;
				struct ltt_channel_struct *ltt_channel = (struct ltt_channel_struct *)rchan->private_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					char *reply;

					DBG("the shmid for the requested channel is %d", rbuf->shmid);
					DBG("the shmid for its buffer structure is %d", ltt_channel->buf_shmid);
					asprintf(&reply, "%d %d", rbuf->shmid, ltt_channel->buf_shmid);

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: get_shmid: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "get_n_subbufs", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_n_subbufs");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_n_subbufs: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				ERR("cannot find trace!");
				return (void *)1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					char *reply;

					DBG("the n_subbufs for the requested channel is %zd", rchan->n_subbufs);
					asprintf(&reply, "%zd", rchan->n_subbufs);

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: get_n_subbufs: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "get_subbuf_size", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_subbuf_size");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_subbuf_size: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				ERR("cannot find trace!");
				return (void *)1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					char *reply;

					DBG("the subbuf_size for the requested channel is %zd", rchan->subbuf_size);
					asprintf(&reply, "%zd", rchan->subbuf_size);

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: get_subbuf_size: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "load_probe_lib", 0) == 1) {
			char *libfile;

			libfile = nth_token(recvbuf, 1);

			DBG("load_probe_lib loading %s", libfile);
		}
		else if(nth_token_is(recvbuf, "get_subbuffer", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;

			DBG("get_subbuf");

			channel_name = nth_token(recvbuf, 1);
			if(channel_name == NULL) {
				ERR("get_subbuf: cannot parse channel");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				ERR("cannot find trace!");
				return (void *)1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					struct rchan_buf *rbuf = rchan->buf;
					struct ltt_channel_buf_struct *lttbuf = trace->channels[i].buf;
					struct blocked_consumer *bc;

					bc = (struct blocked_consumer *) malloc(sizeof(struct blocked_consumer));
					if(bc == NULL) {
						ERR("malloc returned NULL");
						goto next_cmd;
					}
					bc->fd_consumer = src.fd;
					bc->fd_producer = lttbuf->data_ready_fd_read;
					bc->rbuf = rbuf;
					bc->lttbuf = lttbuf;
					bc->src = src;
					bc->server = ustcomm_app.server;

					list_add(&bc->list, &blocked_consumers);

					break;
				}
			}
		}
		else if(nth_token_is(recvbuf, "put_subbuffer", 0) == 1) {
			struct ltt_trace_struct *trace;
			char trace_name[] = "auto";
			int i;
			char *channel_name;
			long consumed_old;
			char *consumed_old_str;
			char *endptr;

			DBG("put_subbuf");

			channel_name = strdup_malloc(nth_token(recvbuf, 1));
			if(channel_name == NULL) {
				ERR("put_subbuf_size: cannot parse channel");
				goto next_cmd;
			}

			consumed_old_str = strdup_malloc(nth_token(recvbuf, 2));
			if(consumed_old_str == NULL) {
				ERR("put_subbuf: cannot parse consumed_old");
				goto next_cmd;
			}
			consumed_old = strtol(consumed_old_str, &endptr, 10);
			if(*endptr != '\0') {
				ERR("put_subbuf: invalid value for consumed_old");
				goto next_cmd;
			}

			ltt_lock_traces();
			trace = _ltt_trace_find(trace_name);
			ltt_unlock_traces();

			if(trace == NULL) {
				ERR("cannot find trace!");
				return (void *)1;
			}

			for(i=0; i<trace->nr_channels; i++) {
				struct rchan *rchan = trace->channels[i].trans_channel_data;

				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
					struct rchan_buf *rbuf = rchan->buf;
					struct ltt_channel_buf_struct *lttbuf = trace->channels[i].buf;
					char *reply;
					long consumed_old=0;

					result = ltt_do_put_subbuf(rbuf, lttbuf, consumed_old);
					if(result < 0) {
						WARN("ltt_do_put_subbuf: error (subbuf=%s)", channel_name);
						asprintf(&reply, "%s", "ERROR");
					}
					else {
						DBG("ltt_do_put_subbuf: success (subbuf=%s)", channel_name);
						asprintf(&reply, "%s", "OK");
					}

					result = ustcomm_send_reply(&ustcomm_app.server, reply, &src);
					if(result) {
						ERR("listener: put_subbuf: ustcomm_send_reply failed");
						goto next_cmd;
					}

					free(reply);

					break;
				}
			}

			free(channel_name);
			free(consumed_old_str);
		}
		else if(nth_token_is(recvbuf, "enable_marker", 0) == 1) {
			char *channel_slash_name = nth_token(recvbuf, 1);
			char channel_name[256]="";
			char marker_name[256]="";

			result = sscanf(channel_slash_name, "%255[^/]/%255s", channel_name, marker_name);

			if(channel_name == NULL || marker_name == NULL) {
				WARN("invalid marker name");
				goto next_cmd;
			}
			printf("%s %s\n", channel_name, marker_name);

			result = ltt_marker_connect(channel_name, marker_name, "default");
			if(result < 0) {
				WARN("could not enable marker; channel=%s, name=%s", channel_name, marker_name);
			}
		}
		else if(nth_token_is(recvbuf, "disable_marker", 0) == 1) {
			char *channel_slash_name = nth_token(recvbuf, 1);
			char *marker_name;
			char *channel_name;

			result = sscanf(channel_slash_name, "%a[^/]/%as", &channel_name, &marker_name);

			if(marker_name == NULL) {
			}
			printf("%s %s\n", channel_name, marker_name);

			result = ltt_marker_disconnect(channel_name, marker_name, "default");
			if(result < 0) {
				WARN("could not disable marker; channel=%s, name=%s", channel_name, marker_name);
			}
		}
//		else if(nth_token_is(recvbuf, "get_notifications", 0) == 1) {
//			struct ltt_trace_struct *trace;
//			char trace_name[] = "auto";
//			int i;
//			char *channel_name;
//
//			DBG("get_notifications");
//
//			channel_name = strdup_malloc(nth_token(recvbuf, 1));
//			if(channel_name == NULL) {
//				ERR("put_subbuf_size: cannot parse channel");
//				goto next_cmd;
//			}
//
//			ltt_lock_traces();
//			trace = _ltt_trace_find(trace_name);
//			ltt_unlock_traces();
//
//			if(trace == NULL) {
//				ERR("cannot find trace!");
//				return (void *)1;
//			}
//
//			for(i=0; i<trace->nr_channels; i++) {
//				struct rchan *rchan = trace->channels[i].trans_channel_data;
//				int fd;
//
//				if(!strcmp(trace->channels[i].channel_name, channel_name)) {
//					struct rchan_buf *rbuf = rchan->buf;
//					struct ltt_channel_buf_struct *lttbuf = trace->channels[i].buf;
//
//					result = fd = ustcomm_app_detach_client(&ustcomm_app, &src);
//					if(result == -1) {
//						ERR("ustcomm_app_detach_client failed");
//						goto next_cmd;
//					}
//
//					lttbuf->wake_consumer_arg = (void *) fd;
//
//					smp_wmb();
//
//					lttbuf->call_wake_consumer = 1;
//
//					break;
//				}
//			}
//
//			free(channel_name);
//		}
		else {
			ERR("unable to parse message: %s", recvbuf);
		}

	next_cmd:
		free(recvbuf);
	}
}

void create_listener(void)
{
#ifdef USE_CLONE
	static char listener_stack[16384];
#endif

#ifdef USE_CLONE
	result = clone(listener_main, listener_stack+sizeof(listener_stack)-1, CLONE_FS | CLONE_FILES | CLONE_VM | CLONE_SIGHAND | CLONE_THREAD, NULL);
	if(result == -1) {
		perror("clone");
	}
#else
	pthread_t thread;

	pthread_create(&thread, NULL, listener_main, NULL);
#endif
}

/* The signal handler itself. Signals must be setup so there cannot be
   nested signals. */

void sighandler(int sig)
{
	static char have_listener = 0;
	DBG("sighandler");

	if(!have_listener) {
		create_listener();
		have_listener = 1;
	}
}

/* Called by the app signal handler to chain it to us. */

void chain_signal(void)
{
	sighandler(USTSIGNAL);
}

static int init_socket(void)
{
	return ustcomm_init_app(getpid(), &ustcomm_app);
}

/* FIXME: reenable this to delete socket file. */

#if 0
static void destroy_socket(void)
{
	int result;

	if(mysocketfile[0] == '\0')
		return;

	result = unlink(mysocketfile);
	if(result == -1) {
		PERROR("unlink");
	}
}
#endif

static int init_signal_handler(void)
{
	/* Attempt to handler SIGIO. If the main program wants to
	 * handle it, fine, it'll override us. They it'll have to
	 * use the chaining function.
	 */

	int result;
	struct sigaction act;

	result = sigemptyset(&act.sa_mask);
	if(result == -1) {
		PERROR("sigemptyset");
		return -1;
	}

	act.sa_handler = sighandler;
	act.sa_flags = SA_RESTART;

	/* Only defer ourselves. Also, try to restart interrupted
	 * syscalls to disturb the traced program as little as possible.
	 */
	result = sigaction(SIGIO, &act, NULL);
	if(result == -1) {
		PERROR("sigaction");
		return -1;
	}

	return 0;
}

static void auto_probe_connect(struct marker *m)
{
	int result;

	result = ltt_marker_connect(m->channel, m->name, "default");
	if(result && result != -EEXIST)
		ERR("ltt_marker_connect (marker = %s/%s, errno = %d)", m->channel, m->name, -result);

	DBG("just auto connected marker %s %s to probe default", m->channel, m->name);
}

static void __attribute__((constructor(101))) init0()
{
	DBG("UST_AUTOPROBE constructor");
	if(getenv("UST_AUTOPROBE")) {
		marker_set_new_marker_cb(auto_probe_connect);
	}
}

static void __attribute__((constructor(1000))) init()
{
	int result;

	DBG("UST_TRACE constructor");

	/* Must create socket before signal handler to prevent races.
         */
	result = init_socket();
	if(result == -1) {
		ERR("init_socket error");
		return;
	}
	result = init_signal_handler();
	if(result == -1) {
		ERR("init_signal_handler error");
		return;
	}

	if(getenv("UST_TRACE")) {
		char trace_name[] = "auto";
		char trace_type[] = "ustrelay";

		DBG("starting early tracing");

		/* Ensure marker control is initialized */
		init_marker_control();

		/* Ensure relay is initialized */
		init_ustrelay_transport();

		/* Ensure markers are initialized */
		init_markers();

		/* In case. */
		ltt_channels_register("ust");

		result = ltt_trace_setup(trace_name);
		if(result < 0) {
			ERR("ltt_trace_setup failed");
			return;
		}

		result = ltt_trace_set_type(trace_name, trace_type);
		if(result < 0) {
			ERR("ltt_trace_set_type failed");
			return;
		}

		result = ltt_trace_alloc(trace_name);
		if(result < 0) {
			ERR("ltt_trace_alloc failed");
			return;
		}

		result = ltt_trace_start(trace_name);
		if(result < 0) {
			ERR("ltt_trace_start failed");
			return;
		}
		inform_consumer_daemon();
	}


	return;

	/* should decrementally destroy stuff if error */

}

/* This is only called if we terminate normally, not with an unhandled signal,
 * so we cannot rely on it. */

/* This destructor probably isn't needed, because ustd can do crash recovery. */
#if 0
static void __attribute__((destructor)) fini()
{
	int result;

	/* if trace running, finish it */

	DBG("destructor stopping traces");

	result = ltt_trace_stop("auto");
	if(result == -1) {
		ERR("ltt_trace_stop error");
	}

	result = ltt_trace_destroy("auto");
	if(result == -1) {
		ERR("ltt_trace_destroy error");
	}

	destroy_socket();
}
#endif
