#define _XOPEN_SOURCE	700
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <fcntl.h>
#include <time.h>
#include <mhash.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <linux/joystick.h>
#include <stdbool.h>
#include <pthread.h>
#include <signal.h>
#include <getopt.h>

#include "js_packet.h"
#include "rctl_config.h"
#include "rctl_link.h"
#include "util.h"
#include "edvs.h"

static bool running = true;

int
open_joystick(const char *js_device) {
	int js_fd = open(js_device, O_RDONLY);
	if (js_fd == -1)
		die("Could not open joystick '%s'.\n", js_device);
	fcntl(js_fd, F_SETFL, O_NONBLOCK);
	return js_fd;
}

int
open_uart(const char *uart_device) {
	int fd = open(uart_device, O_RDONLY);
	if (fd == -1)
		fprintf(stderr, "Could not open UART device '%s'.\n", uart_device);
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return -1;
}

void
sigint_handler(int signum) {
	if (signum == SIGINT)
		running = false;
}

/*
 * mavlink_msg_handler is triggered by a separate thread within the rctl
 * functions! make sure to use mutexes if necessary
 */
void
mavlink_msg_handler(mavlink_message_t msg) {
	// printf("ding %d\n", msg.msgid);
	if (msg.msgid == MAVLINK_MSG_ID_HIGHRES_IMU) {
		mavlink_highres_imu_t imu;
		mavlink_msg_highres_imu_decode(&msg, &imu);
		printf("%12ld %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f\n",
				microsSinceEpoch(),
				imu.xacc,
				imu.yacc,
				imu.zacc,
				imu.xgyro,
				imu.ygyro,
				imu.zgyro,
				imu.xmag,
				imu.ymag,
				imu.zmag);
	}
}

#define BUTTON1_UNCHANGED (1 << 0)
#define BUTTON1_PRESSED   (1 << 1)
#define BUTTON1_RELEASED  (1 << 2)
#define BUTTON2_UNCHANGED (1 << 3)
#define BUTTON2_PRESSED   (1 << 4)
#define BUTTON2_RELEASED  (1 << 5)

int
parse_buttons(int lb, int rb) {
	static int old_lb = INT16_MIN;
	static int old_rb = INT16_MIN;

	int tmp1 = lb - old_lb;
	int tmp2 = rb - old_rb;

	old_lb = lb;
	old_rb = rb;

	int result = 0;
	if (tmp1 > 0) {
		result |= BUTTON1_PRESSED;
	}
	else if (tmp1 < 0) {
		result |= BUTTON1_RELEASED;
	}
	else {
		result |= BUTTON1_UNCHANGED;
	}

	if (tmp2 > 0) {
		result |= BUTTON2_PRESSED;
	}
	else if (tmp2 < 0) {
		result |= BUTTON2_RELEASED;
	}
	else {
		result |= BUTTON2_UNCHANGED;
	}

	return result;
}

void
mainloop(int js_fd, int uart_fd, rctl_link_t *link) {
	int16_t r, p, y, t, lb, rb;
	struct js_event js;
	uint64_t last_time_stamp = microsSinceEpoch();
	bool read_uart_commands = false;

	memset(&js, 0, sizeof(js));
	r = p = y = 0;
	t = lb = rb = INT16_MIN;

	// empty the joystick file buffer
	while (read(js_fd, &js, sizeof(js)) > 0);

	// run main loop
	while (running) {
		if (read(js_fd, &js, sizeof(js)) > 0) {
			switch (js.type & ~JS_EVENT_INIT) {
			case JS_EVENT_AXIS:
				switch (js.number) {
				case YAW:
					y = (int16_t)js.value;
					break;
				case ROLL:
					r = (int16_t)js.value;
					break;
				case PITCH:
					p = (int16_t)js.value;
					break;
				case THROTTLE:
					t = (int16_t)js.value;
					break;
				case LEFTBUT:
					lb = (int16_t)js.value;
					break;
				case RIGHTBUT:
					rb = (int16_t)js.value;
					break;
				default:
					break;
				}
				break;
			}
		}
		int bstate = parse_buttons(lb, rb);
		if (bstate & BUTTON1_PRESSED) {
			rctl_toggle_armed(link);
		}
		if (bstate & BUTTON2_PRESSED) {
			read_uart_commands = !read_uart_commands;
		}

		if (read_uart_commands && (uart_fd > 0)) {
			// TODO read and overwrite commands from uart
		}

		// send commands to drone
		if (running && (microsSinceEpoch() - last_time_stamp > 40000)) {
			last_time_stamp = microsSinceEpoch();
			rctl_set_rpyt(link, r, p, y, t);
		}
	}
}

void
usage(FILE *stream) {
	fprintf(stream, "Usage: px4remotectrl [-d dev] [-i ip] [-j port] [-m port] [-u uart]\n"
			"  -d   Joystick Device (default /dev/input/js0)\n"
			"  -i   Target/MAV IPv4 (default 127.0.0.1)\n"
			"  -j   Joystick port on MAV (default 56000)\n"
			"  -m   Mavlink port on MAV (default 56001)\n"
			"  -u   UART port to read commands from (default /dev/ttyUSB0)\n");
}

void
parse_argv(rctl_config_t *cfg, int argc, char *argv[], char **jdev, char **uart) {
	int opt, slen;
	while ((opt = getopt(argc, argv, "hi:j:m:d:u:")) != -1) {
		switch (opt) {
		case 'i':
			slen = strlen(optarg);
			cfg->target_ip4 = calloc(slen + 1, sizeof(char));
			strncpy(cfg->target_ip4, optarg, slen);
			break;
		case 'j':
			cfg->joystick_port = atoi(optarg);
			break;
		case 'm':
			cfg->mavlink_port = atoi(optarg);
			break;
		case 'd':
			slen = strlen(optarg);
			*jdev = calloc(slen + 1, sizeof(char));
			strncpy(*jdev, optarg, slen);
			break;
		case 'u':
			slen = strlen(optarg);
			*uart = calloc(slen + 1, sizeof(char));
			strncpy(*uart, optarg, slen);
			break;

		case 'h':
			usage(stdout);
			exit(EXIT_SUCCESS);
		default:
			usage(stderr);
			exit(EXIT_FAILURE);
		}
	}
}

int
main(int argc, char *argv[]) {
	/*
	 * register sigint handler to make it possible to close all open
	 * sockets and exit in a sane state
	 */
	struct sigaction sa;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sigint_handler;
	sigaction(SIGINT, &sa, NULL);

	/*
	 * allocate required remote control variables
	 */
	rctl_config_t *cfg = NULL;
	rctl_link_t *link = NULL;
	rctl_alloc_config(&cfg);
	rctl_alloc_link(&link);

	/*
	 * setup the default configuration
	 */
	cfg->target_ip4		= "127.0.0.1";
	cfg->joystick_port	= 56000;
	cfg->mavlink_port	= 56001;
	cfg->system_id		= 255;
	cfg->system_comp	= 0;
	cfg->target_id		= 1;
	cfg->target_comp	= 0;
	cfg->mavlink_handler	= mavlink_msg_handler;
	char *jdev		= "/dev/input/js0";
	char *uart_dev		= "/dev/ttyUSB0";

	/*
	 * parse arguments, if any
	 */
	if (argc > 1)
		parse_argv(cfg, argc, argv, &jdev, &uart_dev);

	/*
	 * open joystick, possibly UART
	 */
	int js_fd = open_joystick(jdev);
	int uart_fd = open_uart(uart_dev);

	/*
	 * main part of the application
	 */
	rctl_connect_mav(cfg, link);
	edvs_start(uart_fd);
	mainloop(js_fd, uart_fd, link);
	rctl_disarm(link);
	rctl_disconnect_mav(link);

	/*
	 * free and close
	 */
	edvs_stop();
	rctl_free_link(&link);
	rctl_free_config(&cfg);
	if (js_fd >= 0) close(js_fd);
	if (uart_fd >= 0) close(uart_fd);
}

