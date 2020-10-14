#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>

#include "wlr-gamma-control-unstable-v1-client-protocol.h"
#include "color_math.h"

#if defined(SPEEDRUN)
static time_t start = 0;
static void init_time(time_t *tloc) {
	(void)tloc;
	tzset();
	struct timespec realtime;
	clock_gettime(CLOCK_REALTIME, &realtime);
	start = realtime.tv_sec;
}
static time_t get_time_sec(time_t *tloc) {
	(void)tloc;
	struct timespec realtime;
	clock_gettime(CLOCK_REALTIME, &realtime);
	time_t now = start + ((realtime.tv_sec - start) * 1000 + realtime.tv_nsec / 1000000);

	struct tm tm;
	localtime_r(&now, &tm);
	fprintf(stderr, "time in termina: %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
	return now;
}
static int set_timer(timer_t timer, time_t deadline) {
	time_t diff = deadline - start;
	struct itimerspec timerspec = {{0}, {0}};
	timerspec.it_value.tv_sec = start + diff / 1000;
	timerspec.it_value.tv_nsec = (diff % 1000) * 1000000;

	struct tm tm;
	localtime_r(&deadline, &tm);
	fprintf(stderr, "sleeping until %02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);

	return timer_settime(timer, TIMER_ABSTIME, &timerspec, NULL);
}
#else
static inline void init_time(time_t *tloc) {
	(void)tloc;
	tzset();
}
static inline time_t get_time_sec(time_t *tloc) {
	return time(tloc);
}
static int set_timer(timer_t timer, time_t deadline) {
	struct itimerspec timerspec = {{0}, {0}};
	timerspec.it_value.tv_sec = deadline;
	return timer_settime(timer, TIMER_ABSTIME, &timerspec, NULL);
}
#endif

struct config {
	int high_temp;
	int low_temp;
	int duration;
	double longitude;
	double latitude;
};

struct context {
	struct config config;
	struct sun sun;

	time_t dawn_step_time;
	time_t dusk_step_time;

	bool new_output;
	struct wl_list outputs;
	timer_t timer;
};

struct output {
	struct wl_list link;

	struct context *context;
	struct wl_output *wl_output;
	struct zwlr_gamma_control_v1 *gamma_control;

	int table_fd;
	uint32_t id;
	uint32_t ramp_size;
	uint16_t *table;
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
	if (output->table_fd != -1) {
		close(output->table_fd);
	}
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
	fprintf(stderr, "gamma control of output %d failed\n",
			output->id);
	zwlr_gamma_control_v1_destroy(output->gamma_control);
	output->gamma_control = NULL;
	if (output->table_fd != -1) {
		close(output->table_fd);
		output->table_fd = -1;
	}
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
		fprintf(stderr, "registry: adding output %d\n", name);
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
			fprintf(stderr, "registry: removing output %d\n", name);
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

static void set_temperature(struct wl_list *outputs, int temp, int gamma) {
	double rw, gw, bw;
	calc_whitepoint(temp, &rw, &gw, &bw);
	fprintf(stderr, "setting temperature to %d K\n", temp);

	struct output *output;
	wl_list_for_each(output, outputs, link) {
		if (output->gamma_control == NULL || output->table_fd == -1) {
			continue;
		}
		fill_gamma_table(output->table, output->ramp_size,
				rw, gw, bw, gamma);
		lseek(output->table_fd, 0, SEEK_SET);
		zwlr_gamma_control_v1_set_gamma(output->gamma_control,
				output->table_fd);
	}
}

static int anim_kelvin_step = 25;

static void recalc_stops(struct context *ctx, time_t now) {
	if (now < ctx->sun.dusk) {
		return;
	}

	time_t day = now - (now % 86400);
	if (day < ctx->sun.dusk) {
		day += 86400;
	}

	struct tm tm = { 0 };
	gmtime_r(&day, &tm);
	calc_sun(&tm, ctx->config.longitude, ctx->config.latitude, &ctx->sun);
	if (ctx->config.duration != -1) {
		ctx->sun.dawn = ctx->sun.sunrise - ctx->config.duration;
		ctx->sun.dusk = ctx->sun.sunset + ctx->config.duration;
	}

	// TODO: Cap on eternal days?
	ctx->sun.dawn += day;
	ctx->sun.sunrise += day;
	ctx->sun.sunset += day;
	ctx->sun.dusk += day;
	assert(ctx->sun.dusk > now);

	int temp_diff = ctx->config.high_temp - ctx->config.low_temp;
	ctx->dawn_step_time = (ctx->sun.sunrise - ctx->sun.dawn) * anim_kelvin_step / temp_diff;
	ctx->dusk_step_time = (ctx->sun.dusk - ctx->sun.sunset) * anim_kelvin_step / temp_diff;

	struct tm dawn, sunrise, sunset, dusk;
	localtime_r(&ctx->sun.dawn, &dawn);
	localtime_r(&ctx->sun.sunrise, &sunrise);
	localtime_r(&ctx->sun.sunset, &sunset);
	localtime_r(&ctx->sun.dusk, &dusk);
	fprintf(stderr, "calculated new sun trajectory: dawn %02d:%02d, sunrise %02d:%02d, sunset %02d:%02d, dusk %02d:%02d\n",
			dawn.tm_hour, dawn.tm_min,
			sunrise.tm_hour, sunrise.tm_min,
			sunset.tm_hour, sunset.tm_min,
			dusk.tm_hour, dusk.tm_min);
}

static int interpolate_temperature(time_t now, time_t start, time_t stop, int temp_start, int temp_stop) {
	double time_pos = clamp((double)(now - start) / (double)(stop - start));
	int temp_pos = (double)(temp_stop - temp_start) * time_pos;
	return temp_start + temp_pos;
}

static int get_temperature(const struct context *ctx, time_t now) {
	if (now < ctx->sun.dawn) {
		return ctx->config.low_temp;
	} else if (now < ctx->sun.sunrise) {
		return interpolate_temperature(now, ctx->sun.dawn, ctx->sun.sunrise, ctx->config.low_temp, ctx->config.high_temp);
	} else if (now < ctx->sun.sunset) {
		return ctx->config.high_temp;
	} else if (now < ctx->sun.dusk) {
		return interpolate_temperature(now, ctx->sun.sunset, ctx->sun.dusk, ctx->config.high_temp, ctx->config.low_temp);
	} else {
		return ctx->config.low_temp;
	}
}

static void update_timer(struct context *ctx, timer_t timer, time_t now) {
	assert(now < ctx->sun.dusk);

	time_t deadline;
	if (now < ctx->sun.dawn) {
		deadline = ctx->sun.dawn;
	} else if (now < ctx->sun.sunrise) {
		deadline = now + ctx->dawn_step_time;
	} else if (now < ctx->sun.sunset) {
		deadline = ctx->sun.sunset;
	} else if (now < ctx->sun.dusk) {
		deadline = now + ctx->dusk_step_time;
	}

	assert(deadline > now);
	set_timer(timer, deadline);
}

static int display_poll(struct wl_display *display, short int events, int timeout) {
	struct pollfd pfd[1];
	pfd[0].fd = wl_display_get_fd(display);
	pfd[0].events = events;
	if (poll(pfd, 1, timeout) == -1) {
		return errno == EINTR ? 0 : -1;
	}
	return 0;
}

static int display_dispatch(struct wl_display *display, int timeout) {
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

static int timer_fired = 0;

static void timer_signal(int signal) {
	(void)signal;
	timer_fired = true;
}

int main(int argc, char *argv[]) {

	init_time(NULL);

	// Initialize defaults
	struct context ctx = {
		.sun = { 0 },
		.config = {
			.high_temp = 6500,
			.low_temp = 4000,
			.duration = -1,
			.latitude = 0,
			.longitude = 0,
		}
	};
	double gamma = 1.0;
	wl_list_init(&ctx.outputs);

	struct sigaction timer_action = {
		.sa_handler = timer_signal,
		.sa_flags = 0,
	};
	if (sigaction(SIGALRM, &timer_action, NULL) == -1) {
		fprintf(stderr, "could not configure alarm handler: %s\n", strerror(errno));
		return 1;
	}

	if (timer_create(CLOCK_REALTIME, NULL, &ctx.timer) == -1) {
		fprintf(stderr, "could not configure timer: %s\n", strerror(errno));
		return 1;
	}

#ifdef SPEEDRUN
	fprintf(stderr, "warning: speedrun mode enabled\n");
#endif

	int opt;
	while ((opt = getopt(argc, argv, "hT:t:g:d:l:L:")) != -1) {
		switch (opt) {
			case 'T':
				ctx.config.high_temp = strtol(optarg, NULL, 10);
				break;
			case 't':
				ctx.config.low_temp = strtol(optarg, NULL, 10);
				break;
			case 'l':
				ctx.config.latitude = strtod(optarg, NULL);
				break;
			case 'L':
				ctx.config.longitude = strtod(optarg, NULL);
				break;
			case 'd':
				fprintf(stderr, "using animation duration override\n");
				ctx.config.duration = strtod(optarg, NULL) * 60;
				break;
			case 'g':
				gamma = strtod(optarg, NULL);
				break;
			case 'h':
			default:
				fprintf(stderr, usage, argv[0]);
				return opt == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (ctx.config.high_temp == ctx.config.low_temp) {
		fprintf(stderr, "high (%d) and low (%d) temperature must not be identical\n",
				ctx.config.high_temp, ctx.config.low_temp);
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
	recalc_stops(&ctx, now);

	int temp = get_temperature(&ctx, now);
	set_temperature(&ctx.outputs, temp, gamma);
	update_timer(&ctx, ctx.timer, now);

	int old_temp = temp;
	while (display_dispatch(display, -1) != -1) {
		if (timer_fired) {
			timer_fired = false;

			now = get_time_sec(NULL);
			recalc_stops(&ctx, now);
			update_timer(&ctx, ctx.timer, now);

			if ((temp = get_temperature(&ctx, now)) != old_temp) {
				old_temp = temp;
				ctx.new_output = false;
				set_temperature(&ctx.outputs, temp, gamma);
			}
		} else if (ctx.new_output) {
			ctx.new_output = false;
			set_temperature(&ctx.outputs, temp, gamma);
		}
	}

	return EXIT_SUCCESS;
}
