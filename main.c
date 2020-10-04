#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <time.h>
#include <poll.h>
#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "color_math.h"

#if defined(SPEEDRUN)
static inline int wait_adjust(int wait) {
	fprintf(stderr, "wait in termina: %d seconds\n", wait / 1000);
	return wait / 1000;
}
static time_t get_time_sec(time_t *tloc) {
	static time_t start = 0;
	static time_t monostart = 0;
	if (start == 0) {
		start = time(tloc);
	}

	struct timespec monotime;
	clock_gettime(CLOCK_MONOTONIC, &monotime);
	time_t now = monotime.tv_sec * 1000 + monotime.tv_nsec / 1000000;
	if (monostart == 0) {
		monostart = now;
	}
	now = now - monostart + start;

	struct tm tm;
	localtime_r(&now, &tm);
	fprintf(stderr, "time in termina: %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
	return now;
}
#else
static inline int wait_adjust(int wait) {
	return wait;
}
static inline time_t get_time_sec(time_t *tloc) {
	return time(tloc);
}
#endif

static const int LONG_SLEEP_MS = 600 * 1000;
static const int MAX_SLEEP_S = 1800;
static const int MIN_SLEEP_S = 10;

enum state {
	HIGH_TEMP,
	ANIMATING_TO_LOW,
	LOW_TEMP,
	ANIMATING_TO_HIGH,
};

static char *state_names[] = {
	"high temperature",
	"animating to low temperature",
	"low temperature",
	"animating to high temperature",
	NULL
};

struct context {
	double gamma;

	int high_temp;
	int low_temp;
	int duration;
	double longitude;
	double latitude;

	time_t start_time;
	time_t stop_time;
	int cur_temp;
	enum state state;
	bool new_output;

	struct wl_list outputs;
};

struct output {
	struct context *context;
	struct wl_output *wl_output;
	uint32_t id;
	struct zwlr_gamma_control_v1 *gamma_control;
	uint32_t ramp_size;
	int table_fd;
	uint16_t *table;
	struct wl_list link;
};

static struct zwlr_gamma_control_manager_v1 *gamma_control_manager = NULL;

static int create_anonymous_file(off_t size) {
	char template[] = "/tmp/wlsunset-shared-XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0) {
		return -1;
	}

	int ret;
	do {
		errno = 0;
		ret = ftruncate(fd, size);
	} while (errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}

	unlink(template);
	return fd;
}

static int create_gamma_table(uint32_t ramp_size, uint16_t **table) {
	size_t table_size = ramp_size * 3 * sizeof(uint16_t);
	int fd = create_anonymous_file(table_size);
	if (fd < 0) {
		fprintf(stderr, "failed to create anonymous file\n");
		return -1;
	}

	void *data =
		mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "failed to mmap()\n");
		close(fd);
		return -1;
	}

	*table = data;
	return fd;
}

static void gamma_control_handle_gamma_size(void *data,
		struct zwlr_gamma_control_v1 *gamma_control, uint32_t ramp_size) {
	(void)gamma_control;
	struct output *output = data;
	output->ramp_size = ramp_size;
	output->table_fd = create_gamma_table(ramp_size, &output->table);
	output->context->new_output = true;
	if (output->table_fd < 0) {
		fprintf(stderr, "could not create gamma table for output %d\n",
				output->id);
		exit(EXIT_FAILURE);
	}
}

static void gamma_control_handle_failed(void *data,
		struct zwlr_gamma_control_v1 *gamma_control) {
	(void)gamma_control;
	struct output *output = data;
	fprintf(stderr, "failed to set gamma table for output %d\n",
			output->id);
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.gamma_size = gamma_control_handle_gamma_size,
	.failed = gamma_control_handle_failed,
};

static void setup_output(struct output *output) {
	if (output->gamma_control != NULL) {
		return;
	}
	if (gamma_control_manager == NULL) {
		fprintf(stderr, "skipping setup of output %d: gamma_control_manager missing\n",
				output->id);
		return;
	}
	output->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
		gamma_control_manager, output->wl_output);
	zwlr_gamma_control_v1_add_listener(output->gamma_control,
		&gamma_control_listener, output);
}

static void registry_handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	(void)version;
	struct context *ctx = (struct context *)data;
	if (strcmp(interface, wl_output_interface.name) == 0) {
		fprintf(stderr, "adding output %d\n", name);
		struct output *output = calloc(1, sizeof(struct output));
		output->id = name;
		output->wl_output = wl_registry_bind(registry, name,
				&wl_output_interface, 1);
		output->table_fd = -1;
		output->context = ctx;
		wl_list_insert(&ctx->outputs, &output->link);
		setup_output(output);
	} else if (strcmp(interface,
				zwlr_gamma_control_manager_v1_interface.name) == 0) {
		gamma_control_manager = wl_registry_bind(registry, name,
				&zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void registry_handle_global_remove(void *data,
		struct wl_registry *registry, uint32_t name) {
	(void)registry;
	struct context *ctx = (struct context *)data;
	struct output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &ctx->outputs, link) {
		if (output->id == name) {
			fprintf(stderr, "removing output %d\n", name);
			wl_list_remove(&output->link);
			if (output->gamma_control != NULL) {
				zwlr_gamma_control_v1_destroy(output->gamma_control);
			}
			if (output->table_fd != -1) {
				close(output->table_fd);
			}
			free(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static void fill_gamma_table(uint16_t *table, uint32_t ramp_size, double rw, double gw, double bw, double gamma) {
	uint16_t *r = table;
	uint16_t *g = table + ramp_size;
	uint16_t *b = table + 2 * ramp_size;
	for (uint32_t i = 0; i < ramp_size; ++i) {
		double val = (double)i / (ramp_size - 1);
		r[i] = (uint16_t)(UINT16_MAX * pow(val * rw, 1.0 / gamma));
		g[i] = (uint16_t)(UINT16_MAX * pow(val * gw, 1.0 / gamma));
		b[i] = (uint16_t)(UINT16_MAX * pow(val * bw, 1.0 / gamma));
	}
}

static void set_temperature(struct context *ctx) {
	double rw, gw, bw;
	calc_whitepoint(ctx->cur_temp, &rw, &gw, &bw);
	fprintf(stderr, "setting temperature: %d\n", ctx->cur_temp);

	struct output *output;
	wl_list_for_each(output, &ctx->outputs, link) {
		if (output->gamma_control == NULL || output->table_fd == -1) {
			continue;
		}
		fill_gamma_table(output->table, output->ramp_size,
				rw, gw, bw, ctx->gamma);
		lseek(output->table_fd, 0, SEEK_SET);
		zwlr_gamma_control_v1_set_gamma(output->gamma_control,
				output->table_fd);
	}
}

static void recalc_stops(struct context *ctx, time_t now) {
	time_t day = now - (now % 86400);
	time_t true_end = ctx->stop_time + ctx->duration;
	if (ctx->stop_time == 0) {
		// First calculation
	} else if (now > true_end) {
		day += 86400;
	} else if (day < true_end) {
		return;
	}

	struct tm tm = { 0 };
	gmtime_r(&now, &tm);
	sun(&tm, ctx->longitude, ctx->latitude, &ctx->start_time, &ctx->stop_time);
	ctx->start_time += day;
	ctx->stop_time += day;

	struct tm sunrise, sunset;
	localtime_r(&ctx->start_time, &sunrise);
	localtime_r(&ctx->stop_time, &sunset);
	fprintf(stderr, "calculated new sun trajectory: sunrise %02d:%02d, sunset %02d:%02d\n",
			sunrise.tm_hour, sunrise.tm_min,
			sunset.tm_hour, sunset.tm_min);

	ctx->stop_time -= ctx->duration;
}

static void update_temperature(struct context *ctx, time_t now) {
	int temp = 0, temp_pos;
	double time_pos;

	recalc_stops(ctx, now);

	enum state old_state = ctx->state;
	switch (ctx->state) {
start:
	case HIGH_TEMP:
		if (now <= ctx->stop_time && now > ctx->start_time + ctx->duration) {
			temp = ctx->high_temp;
			break;
		}
		ctx->state = ANIMATING_TO_LOW;
		// fallthrough
	case ANIMATING_TO_LOW:
		if (now > ctx->start_time && now <= ctx->stop_time + ctx->duration) {
			time_pos = clamp(((double)now - (double)ctx->stop_time) / (double)ctx->duration);
			temp_pos = (double)(ctx->high_temp - ctx->low_temp) * time_pos;
			temp = ctx->high_temp - temp_pos;
			break;
		}
		ctx->state = LOW_TEMP;
		// fallthrough
	case LOW_TEMP:
		if (now > ctx->stop_time + ctx->duration || now <= ctx->start_time) {
			temp = ctx->low_temp;
			break;
		}
		ctx->state = ANIMATING_TO_HIGH;
		// fallthrough
	case ANIMATING_TO_HIGH:
		if (now <= ctx->start_time + ctx->duration) {
			time_pos = clamp(((double)now - (double)ctx->start_time) / (double)ctx->duration);
			temp_pos = (double)(ctx->high_temp - ctx->low_temp) * time_pos;
			temp = ctx->low_temp + temp_pos;
			break;
		}
		ctx->state = HIGH_TEMP;
		goto start;
	}

	if (ctx->state != old_state) {
		fprintf(stderr, "changed state: %s\n", state_names[ctx->state]);
	}

	if (temp != ctx->cur_temp || ctx->new_output) {
		ctx->cur_temp = temp;
		ctx->new_output = false;
		set_temperature(ctx);
	}
}

static int increments(struct context *ctx, int from, int to) {
	int temp_diff = to - from;
	assert(temp_diff > 0);
	int time = ctx->duration * 25000 / temp_diff;
	return time > LONG_SLEEP_MS ? LONG_SLEEP_MS : time;
}

static int time_to_next_event(struct context *ctx, time_t now) {
	time_t deadline;
	switch (ctx->state) {
	case HIGH_TEMP:
		deadline = ctx->stop_time;
		break;
	case LOW_TEMP:
		deadline = ctx->start_time;
		if (deadline < now) {
			deadline = ((deadline / 86400 + 1) * 86400);
		}
		break;
	case ANIMATING_TO_HIGH:
	case ANIMATING_TO_LOW:
		return increments(ctx, ctx->low_temp, ctx->high_temp);
	default:
		return LONG_SLEEP_MS;
	}

	if (deadline <= now) {
		return LONG_SLEEP_MS;
	}

	time_t wait = deadline - now;
	if (wait > MAX_SLEEP_S) {
		wait = MAX_SLEEP_S;
	} else if (wait < MIN_SLEEP_S) {
		wait = MIN_SLEEP_S;
	}
	return wait * 1000;
}

static int display_poll(struct wl_display *display, short int events, int timeout) {
	struct pollfd pfd[1];
	pfd[0].fd = wl_display_get_fd(display);
	pfd[0].events = events;
	return poll(pfd, 1, timeout);
}

static int display_dispatch_with_timeout(struct wl_display *display, int timeout) {
	if (wl_display_prepare_read(display) == -1) {
		return wl_display_dispatch_pending(display);
	}

	int ret;
	while (true) {
		ret = wl_display_flush(display);
		if (ret != -1 || errno != EAGAIN) {
			break;
		}

		if (display_poll(display, POLLOUT, -1) == -1) {
			wl_display_cancel_read(display);
			return -1;
		}
	}

	if (ret < 0 && errno != EPIPE) {
		wl_display_cancel_read(display);
		return -1;
	}

	if (display_poll(display, POLLIN, timeout) == -1) {
		wl_display_cancel_read(display);
		return -1;
	}

	if (wl_display_read_events(display) == -1) {
		return -1;
	}

	return wl_display_dispatch_pending(display);
}

static const char usage[] = "usage: %s [options]\n"
"  -h            show this help message\n"
"  -T <temp>     set high temperature (default: 6500)\n"
"  -t <temp>     set low temperature (default: 4000)\n"
"  -l <lat>      set latitude (e.g. 39.9)\n"
"  -L <long>     set longitude (e.g. 116.3)\n"
"  -d <minutes>  set ramping duration in minutes (default: 60)\n"
"  -g <gamma>    set gamma (default: 1.0)\n";

int main(int argc, char *argv[]) {

	tzset();

	// Initialize defaults
	struct context ctx = {
		.gamma = 1.0,
		.high_temp = 6500,
		.low_temp = 4000,
		.duration = 3600,
		.state = HIGH_TEMP,
	};
	wl_list_init(&ctx.outputs);

	int opt;
	while ((opt = getopt(argc, argv, "hT:t:g:d:l:L:")) != -1) {
		switch (opt) {
			case 'T':
				ctx.high_temp = strtol(optarg, NULL, 10);
				break;
			case 't':
				ctx.low_temp = strtol(optarg, NULL, 10);
				break;
			case 'l':
				ctx.latitude = strtod(optarg, NULL);
				break;
			case 'L':
				ctx.longitude = strtod(optarg, NULL);
				break;
			case 'd':
				ctx.duration = strtod(optarg, NULL) * 60;
				break;
			case 'g':
				ctx.gamma = strtod(optarg, NULL);
				break;
			case 'h':
			default:
				fprintf(stderr, usage, argv[0]);
				return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (ctx.high_temp == ctx.low_temp) {
		fprintf(stderr, "high (%d) and low (%d) temperature must not be identical\n",
				ctx.high_temp, ctx.low_temp);
		return -1;
	}

	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return -1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, &ctx);
	wl_display_roundtrip(display);

	if (gamma_control_manager == NULL) {
		fprintf(stderr,
				"compositor doesn't support wlr-gamma-control-unstable-v1\n");
		return EXIT_FAILURE;
	}

	struct output *output;
	wl_list_for_each(output, &ctx.outputs, link) {
		setup_output(output);
	}
	wl_display_roundtrip(display);


	time_t now = get_time_sec(NULL);
	update_temperature(&ctx, now);
	while (display_dispatch_with_timeout(display, wait_adjust(time_to_next_event(&ctx, now))) != -1) {
		now = get_time_sec(NULL);
		update_temperature(&ctx, now);
	}

	return EXIT_SUCCESS;
}
