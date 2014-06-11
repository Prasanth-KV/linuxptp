/**
 * @file phc2sys.c
 * @brief Utility program to synchronize two clocks via a PPS.
 * @note Copyright (C) 2012 Richard Cochran <richardcochran@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/pps.h>
#include <linux/ptp_clock.h>

#include "clockadj.h"
#include "clockcheck.h"
#include "ds.h"
#include "fsm.h"
#include "missing.h"
#include "phc.h"
#include "pi.h"
#include "pmc_common.h"
#include "print.h"
#include "servo.h"
#include "sk.h"
#include "stats.h"
#include "sysoff.h"
#include "tlv.h"
#include "util.h"
#include "version.h"

#define KP 0.7
#define KI 0.3
#define NS_PER_SEC 1000000000LL

#define PHC_PPS_OFFSET_LIMIT 10000000
#define PMC_UPDATE_INTERVAL (60 * NS_PER_SEC)

struct clock {
	LIST_ENTRY(clock) list;
	clockid_t clkid;
	int sysoff_supported;
	int is_utc;
	struct servo *servo;
	enum servo_state servo_state;
	const char *source_label;
	struct stats *offset_stats;
	struct stats *freq_stats;
	struct stats *delay_stats;
	struct clockcheck *sanity_check;
};

struct node {
	unsigned int stats_max_count;
	int sanity_freq_limit;
	enum servo_type servo_type;
	int phc_readings;
	double phc_interval;
	int sync_offset;
	int forced_sync_offset;
	int leap;
	int leap_set;
	int kernel_leap;
	struct pmc *pmc;
	int pmc_ds_requested;
	uint64_t pmc_last_update;
	LIST_HEAD(clock_head, clock) clocks;
	struct clock *master;
};

static int update_sync_offset(struct node *node);
static int clock_handle_leap(struct node *node, struct clock *clock,
			     int64_t offset, uint64_t ts, int do_leap);

static clockid_t clock_open(char *device)
{
	struct sk_ts_info ts_info;
	char phc_device[16];
	int clkid;

	/* check if device is CLOCK_REALTIME */
	if (!strcasecmp(device, "CLOCK_REALTIME"))
		return CLOCK_REALTIME;

	/* check if device is valid phc device */
	clkid = phc_open(device);
	if (clkid != CLOCK_INVALID)
		return clkid;

	/* check if device is a valid ethernet device */
	if (sk_get_ts_info(device, &ts_info) || !ts_info.valid) {
		fprintf(stderr, "unknown clock %s: %m\n", device);
		return CLOCK_INVALID;
	}

	if (ts_info.phc_index < 0) {
		fprintf(stderr, "interface %s does not have a PHC\n", device);
		return CLOCK_INVALID;
	}

	sprintf(phc_device, "/dev/ptp%d", ts_info.phc_index);
	clkid = phc_open(phc_device);
	if (clkid == CLOCK_INVALID)
		fprintf(stderr, "cannot open %s: %m\n", device);
	return clkid;
}

static int clock_add(struct node *node, clockid_t clkid)
{
	struct clock *c;
	int max_ppb;
	double ppb;

	c = calloc(1, sizeof(*c));
	if (!c) {
		pr_err("failed to allocate memory for a clock");
		return -1;
	}
	c->clkid = clkid;
	c->servo_state = SERVO_UNLOCKED;

	if (c->clkid == CLOCK_REALTIME) {
		c->source_label = "sys";
		c->is_utc = 1;
	} else {
		c->source_label = "phc";
	}

	if (node->stats_max_count > 0) {
		c->offset_stats = stats_create();
		c->freq_stats = stats_create();
		c->delay_stats = stats_create();
		if (!c->offset_stats ||
		    !c->freq_stats ||
		    !c->delay_stats) {
			pr_err("failed to create stats");
			return -1;
		}
	}
	if (node->sanity_freq_limit) {
		c->sanity_check = clockcheck_create(node->sanity_freq_limit);
		if (!c->sanity_check) {
			pr_err("failed to create clock check");
			return -1;
		}
	}

	clockadj_init(c->clkid);
	ppb = clockadj_get_freq(c->clkid);
	/* The reading may silently fail and return 0, reset the frequency to
	   make sure ppb is the actual frequency of the clock. */
	clockadj_set_freq(c->clkid, ppb);
	if (c->clkid == CLOCK_REALTIME) {
		sysclk_set_leap(0);
		max_ppb = sysclk_max_freq();
	} else {
		max_ppb = phc_max_adj(c->clkid);
		if (!max_ppb) {
			pr_err("clock is not adjustable");
			return -1;
		}
	}

	c->servo = servo_create(node->servo_type, -ppb, max_ppb, 0);
	servo_sync_interval(c->servo, node->phc_interval);

	if (clkid != CLOCK_REALTIME)
		c->sysoff_supported = (SYSOFF_SUPPORTED ==
				       sysoff_probe(CLOCKID_TO_FD(clkid),
						    node->phc_readings));

	LIST_INSERT_HEAD(&node->clocks, c, list);
	return 0;
}

static int read_phc(clockid_t clkid, clockid_t sysclk, int readings,
		    int64_t *offset, uint64_t *ts, int64_t *delay)
{
	struct timespec tdst1, tdst2, tsrc;
	int i;
	int64_t interval, best_interval = INT64_MAX;

	/* Pick the quickest clkid reading. */
	for (i = 0; i < readings; i++) {
		if (clock_gettime(sysclk, &tdst1) ||
				clock_gettime(clkid, &tsrc) ||
				clock_gettime(sysclk, &tdst2)) {
			pr_err("failed to read clock: %m");
			return 0;
		}

		interval = (tdst2.tv_sec - tdst1.tv_sec) * NS_PER_SEC +
			tdst2.tv_nsec - tdst1.tv_nsec;

		if (best_interval > interval) {
			best_interval = interval;
			*offset = (tdst1.tv_sec - tsrc.tv_sec) * NS_PER_SEC +
				tdst1.tv_nsec - tsrc.tv_nsec + interval / 2;
			*ts = tdst2.tv_sec * NS_PER_SEC + tdst2.tv_nsec;
		}
	}
	*delay = best_interval;

	return 1;
}

static int64_t get_sync_offset(struct node *node, struct clock *dst)
{
	int direction = node->forced_sync_offset;

	if (!direction)
		direction = dst->is_utc - node->master->is_utc;
	return (int64_t)node->sync_offset * NS_PER_SEC * direction;
}

static void update_clock_stats(struct clock *clock, unsigned int max_count,
			       int64_t offset, double freq, int64_t delay)
{
	struct stats_result offset_stats, freq_stats, delay_stats;

	stats_add_value(clock->offset_stats, offset);
	stats_add_value(clock->freq_stats, freq);
	if (delay >= 0)
		stats_add_value(clock->delay_stats, delay);

	if (stats_get_num_values(clock->offset_stats) < max_count)
		return;

	stats_get_result(clock->offset_stats, &offset_stats);
	stats_get_result(clock->freq_stats, &freq_stats);

	if (!stats_get_result(clock->delay_stats, &delay_stats)) {
		pr_info("rms %4.0f max %4.0f "
			"freq %+6.0f +/- %3.0f "
			"delay %5.0f +/- %3.0f",
			offset_stats.rms, offset_stats.max_abs,
			freq_stats.mean, freq_stats.stddev,
			delay_stats.mean, delay_stats.stddev);
	} else {
		pr_info("rms %4.0f max %4.0f "
			"freq %+6.0f +/- %3.0f",
			offset_stats.rms, offset_stats.max_abs,
			freq_stats.mean, freq_stats.stddev);
	}

	stats_reset(clock->offset_stats);
	stats_reset(clock->freq_stats);
	stats_reset(clock->delay_stats);
}

static void update_clock(struct node *node, struct clock *clock,
			 int64_t offset, uint64_t ts, int64_t delay,
			 int do_leap)
{
	enum servo_state state;
	double ppb;

	if (clock_handle_leap(node, clock, offset, ts, do_leap))
		return;

	offset += get_sync_offset(node, clock);

	if (clock->sanity_check && clockcheck_sample(clock->sanity_check, ts))
		servo_reset(clock->servo);

	ppb = servo_sample(clock->servo, offset, ts, &state);
	clock->servo_state = state;

	switch (state) {
	case SERVO_UNLOCKED:
		break;
	case SERVO_JUMP:
		clockadj_step(clock->clkid, -offset);
		if (clock->sanity_check)
			clockcheck_step(clock->sanity_check, -offset);
		/* Fall through. */
	case SERVO_LOCKED:
		clockadj_set_freq(clock->clkid, -ppb);
		if (clock->clkid == CLOCK_REALTIME)
			sysclk_set_sync();
		if (clock->sanity_check)
			clockcheck_set_freq(clock->sanity_check, -ppb);
		break;
	}

	if (clock->offset_stats) {
		update_clock_stats(clock, node->stats_max_count, offset, ppb, delay);
	} else {
		if (delay >= 0) {
			pr_info("%s offset %9" PRId64 " s%d freq %+7.0f "
				"delay %6" PRId64,
				node->master->source_label, offset, state, ppb, delay);
		} else {
			pr_info("%s offset %9" PRId64 " s%d freq %+7.0f",
				node->master->source_label, offset, state, ppb);
		}
	}
}

static void enable_pps_output(clockid_t src)
{
	int enable = 1;

	if (!phc_has_pps(src))
		return;
	if (ioctl(CLOCKID_TO_FD(src), PTP_ENABLE_PPS, enable) < 0)
		pr_warning("failed to enable PPS output");
}

static int read_pps(int fd, int64_t *offset, uint64_t *ts)
{
	struct pps_fdata pfd;

	pfd.timeout.sec = 10;
	pfd.timeout.nsec = 0;
	pfd.timeout.flags = ~PPS_TIME_INVALID;
	if (ioctl(fd, PPS_FETCH, &pfd)) {
		pr_err("failed to fetch PPS: %m");
		return 0;
	}

	*ts = pfd.info.assert_tu.sec * NS_PER_SEC;
	*ts += pfd.info.assert_tu.nsec;

	*offset = *ts % NS_PER_SEC;
	if (*offset > NS_PER_SEC / 2)
		*offset -= NS_PER_SEC;

	return 1;
}

static int do_pps_loop(struct node *node, struct clock *clock, int fd)
{
	int64_t pps_offset, phc_offset, phc_delay;
	uint64_t pps_ts, phc_ts;
	clockid_t src = node->master->clkid;
	int do_leap;

	node->master->source_label = "pps";

	if (src == CLOCK_INVALID) {
		/* The sync offset can't be applied with PPS alone. */
		node->sync_offset = 0;
	} else {
		enable_pps_output(node->master->clkid);
	}

	while (1) {
		if (!read_pps(fd, &pps_offset, &pps_ts)) {
			continue;
		}

		/* If a PHC is available, use it to get the whole number
		   of seconds in the offset and PPS for the rest. */
		if (src != CLOCK_INVALID) {
			if (!read_phc(src, clock->clkid, node->phc_readings,
				      &phc_offset, &phc_ts, &phc_delay))
				return -1;

			/* Convert the time stamp to the PHC time. */
			phc_ts -= phc_offset;

			/* Check if it is close to the start of the second. */
			if (phc_ts % NS_PER_SEC > PHC_PPS_OFFSET_LIMIT) {
				pr_warning("PPS is not in sync with PHC"
					   " (0.%09lld)", phc_ts % NS_PER_SEC);
				continue;
			}

			phc_ts = phc_ts / NS_PER_SEC * NS_PER_SEC;
			pps_offset = pps_ts - phc_ts;
		}

		do_leap = update_sync_offset(node);
		if (do_leap < 0)
			continue;
		update_clock(node, clock, pps_offset, pps_ts, -1, do_leap);
	}
	close(fd);
	return 0;
}

static int do_loop(struct node *node)
{
	struct timespec interval;
	struct clock *clock;
	uint64_t ts;
	int64_t offset, delay;
	int src_fd = CLOCKID_TO_FD(node->master->clkid);
	int do_leap;

	interval.tv_sec = node->phc_interval;
	interval.tv_nsec = (node->phc_interval - interval.tv_sec) * 1e9;

	while (1) {
		clock_nanosleep(CLOCK_MONOTONIC, 0, &interval, NULL);
		do_leap = update_sync_offset(node);
		if (do_leap < 0)
			continue;

		LIST_FOREACH(clock, &node->clocks, list) {
			if (clock == node->master)
				continue;

			if (clock->clkid == CLOCK_REALTIME &&
			    node->master->sysoff_supported) {
				/* use sysoff */
				if (sysoff_measure(src_fd, node->phc_readings,
						   &offset, &ts, &delay))
					return -1;
			} else {
				/* use phc */
				if (!read_phc(node->master->clkid, clock->clkid,
					      node->phc_readings,
					      &offset, &ts, &delay))
					continue;
			}
			update_clock(node, clock, offset, ts, delay, do_leap);
		}
	}
	return 0; /* unreachable */
}

static int is_msg_mgt(struct ptp_message *msg)
{
	struct TLV *tlv;

	if (msg_type(msg) != MANAGEMENT)
		return 0;
	if (management_action(msg) != RESPONSE)
		return 0;
	if (msg->tlv_count != 1)
		return 0;
	tlv = (struct TLV *) msg->management.suffix;
	if (tlv->type != TLV_MANAGEMENT)
		return 0;
	return 1;
}

static int get_mgt_id(struct ptp_message *msg)
{
	struct management_tlv *mgt = (struct management_tlv *) msg->management.suffix;
	return mgt->id;
}

static void *get_mgt_data(struct ptp_message *msg)
{
	struct management_tlv *mgt = (struct management_tlv *) msg->management.suffix;
	return mgt->data;
}

static int init_pmc(struct node *node, int domain_number)
{
	node->pmc = pmc_create(TRANS_UDS, "/var/run/phc2sys", 0,
				domain_number, 0, 1);
	if (!node->pmc) {
		pr_err("failed to create pmc");
		return -1;
	}

	return 0;
}

static int run_pmc(struct node *node, int timeout, int ds_id,
		   struct ptp_message **msg)
{
#define N_FD 1
	struct pollfd pollfd[N_FD];
	int cnt;

	while (1) {
		pollfd[0].fd = pmc_get_transport_fd(node->pmc);
		pollfd[0].events = POLLIN|POLLPRI;
		if (!node->pmc_ds_requested)
			pollfd[0].events |= POLLOUT;

		cnt = poll(pollfd, N_FD, timeout);
		if (cnt < 0) {
			pr_err("poll failed");
			return -1;
		}
		if (!cnt) {
			/* Request the data set again in the next run. */
			node->pmc_ds_requested = 0;
			return 0;
		}

		/* Send a new request if there are no pending messages. */
		if ((pollfd[0].revents & POLLOUT) &&
		    !(pollfd[0].revents & (POLLIN|POLLPRI))) {
			pmc_send_get_action(node->pmc, ds_id);
			node->pmc_ds_requested = 1;
		}

		if (!(pollfd[0].revents & (POLLIN|POLLPRI)))
			continue;

		*msg = pmc_recv(node->pmc);

		if (!*msg)
			continue;

		if (!is_msg_mgt(*msg) ||
		    get_mgt_id(*msg) != ds_id) {
			msg_put(*msg);
			*msg = NULL;
			continue;
		}
		node->pmc_ds_requested = 0;
		return 1;
	}
}

static int run_pmc_wait_sync(struct node *node, int timeout)
{
	struct ptp_message *msg;
	int res;
	void *data;
	Enumeration8 portState;

	while (1) {
		res = run_pmc(node, timeout, PORT_DATA_SET, &msg);
		if (res <= 0)
			return res;

		data = get_mgt_data(msg);
		portState = ((struct portDS *)data)->portState;
		msg_put(msg);

		switch (portState) {
		case PS_MASTER:
		case PS_SLAVE:
			return 1;
		}
		/* try to get more data sets (for other ports) */
		node->pmc_ds_requested = 1;
	}
}

static int run_pmc_get_utc_offset(struct node *node, int timeout)
{
	struct ptp_message *msg;
	int res;
	struct timePropertiesDS *tds;

	res = run_pmc(node, timeout, TIME_PROPERTIES_DATA_SET, &msg);
	if (res <= 0)
		return res;

	tds = (struct timePropertiesDS *)get_mgt_data(msg);
	if (tds->flags & PTP_TIMESCALE) {
		node->sync_offset = tds->currentUtcOffset;
		if (tds->flags & LEAP_61)
			node->leap = 1;
		else if (tds->flags & LEAP_59)
			node->leap = -1;
		else
			node->leap = 0;
	}
	msg_put(msg);
	return 1;
}

static void close_pmc(struct node *node)
{
	pmc_destroy(node->pmc);
	node->pmc = NULL;
}

/* Returns: -1 in case of error, 0 for normal sync, 1 to leap clock */
static int update_sync_offset(struct node *node)
{
	struct timespec tp;
	uint64_t ts;
	int clock_leap;

	if (clock_gettime(CLOCK_REALTIME, &tp)) {
		pr_err("failed to read clock: %m");
		return -1;
	}
	ts = tp.tv_sec * NS_PER_SEC + tp.tv_nsec;

	if (node->pmc &&
	    !(ts > node->pmc_last_update &&
	      ts - node->pmc_last_update < PMC_UPDATE_INTERVAL)) {
		if (run_pmc_get_utc_offset(node, 0) > 0)
			node->pmc_last_update = ts;
	}

	/* Handle leap seconds. */

	if (!node->leap && !node->leap_set)
		return 0;

	clock_leap = leap_second_status(ts, node->leap_set,
					&node->leap, &node->sync_offset);
	if (node->leap_set != clock_leap) {
		node->leap_set = clock_leap;
		return 1;
	}
	return 0;
}

/* Returns: non-zero to skip clock update */
static int clock_handle_leap(struct node *node, struct clock *clock,
			     int64_t offset, uint64_t ts, int do_leap)
{
	if (!node->leap && !do_leap)
		return 0;

	if (clock->is_utc == node->master->is_utc)
		return 0;

	/* If the system clock is the master clock, get a time stamp from
	   it, as it is the clock which will include the leap second. */
	if (node->master->is_utc) {
		struct timespec tp;
		if (clock_gettime(node->master->clkid, &tp)) {
			pr_err("failed to read clock: %m");
			return -1;
		}
		ts = tp.tv_sec * NS_PER_SEC + tp.tv_nsec;
	}

	/* If the clock will be stepped, the time stamp has to be the
	   target time. Ignore possible 1 second error in UTC offset. */
	if (clock->is_utc && clock->servo_state == SERVO_UNLOCKED)
		ts -= offset + get_sync_offset(node, clock);

	/* Suspend clock updates in the last second before midnight. */
	if (is_utc_ambiguous(ts)) {
		pr_info("clock update suspended due to leap second");
		return 1;
	}

	if (do_leap) {
		/* Only the system clock can leap. */
		if (clock->clkid == CLOCK_REALTIME && node->kernel_leap)
			sysclk_set_leap(node->leap_set);
	}

	return 0;
}

static void usage(char *progname)
{
	fprintf(stderr,
		"\n"
		"usage: %s [options]\n\n"
		" -c [dev|name]  slave clock (CLOCK_REALTIME)\n"
		" -d [dev]       master PPS device\n"
		" -s [dev|name]  master clock\n"
		" -E [pi|linreg] clock servo (pi)\n"
		" -P [kp]        proportional constant (0.7)\n"
		" -I [ki]        integration constant (0.3)\n"
		" -S [step]      step threshold (disabled)\n"
		" -F [step]      step threshold only on start (0.00002)\n"
		" -R [rate]      slave clock update rate in HZ (1.0)\n"
		" -N [num]       number of master clock readings per update (5)\n"
		" -O [offset]    slave-master time offset (0)\n"
		" -L [limit]     sanity frequency limit in ppb (200000000)\n"
		" -u [num]       number of clock updates in summary stats (0)\n"
		" -w             wait for ptp4l\n"
		" -n [num]       domain number (0)\n"
		" -x             apply leap seconds by servo instead of kernel\n"
		" -l [num]       set the logging level to 'num' (6)\n"
		" -m             print messages to stdout\n"
		" -q             do not print messages to the syslog\n"
		" -v             prints the software version and exits\n"
		" -h             prints this message and exits\n"
		"\n",
		progname);
}

int main(int argc, char *argv[])
{
	char *progname;
	clockid_t src = CLOCK_INVALID;
	clockid_t dst = CLOCK_REALTIME;
	int c, domain_number = 0, pps_fd = -1;
	int r, wait_sync = 0;
	int print_level = LOG_INFO, use_syslog = 1, verbose = 0;
	double phc_rate;
	struct node node = {
		.sanity_freq_limit = 200000000,
		.servo_type = CLOCK_SERVO_PI,
		.phc_readings = 5,
		.phc_interval = 1.0,
		.kernel_leap = 1,
	};

	configured_pi_kp = KP;
	configured_pi_ki = KI;

	/* Process the command line arguments. */
	progname = strrchr(argv[0], '/');
	progname = progname ? 1+progname : argv[0];
	while (EOF != (c = getopt(argc, argv,
				  "c:d:s:E:P:I:S:F:R:N:O:L:i:u:wn:xl:mqvh"))) {
		switch (c) {
		case 'c':
			dst = clock_open(optarg);
			break;
		case 'd':
			pps_fd = open(optarg, O_RDONLY);
			if (pps_fd < 0) {
				fprintf(stderr,
					"cannot open '%s': %m\n", optarg);
				return -1;
			}
			break;
		case 'i':
			fprintf(stderr,
				"'-i' has been deprecated. please use '-s' instead.\n");
		case 's':
			src = clock_open(optarg);
			break;
		case 'E':
			if (!strcasecmp(optarg, "pi")) {
				node.servo_type = CLOCK_SERVO_PI;
			} else if (!strcasecmp(optarg, "linreg")) {
				node.servo_type = CLOCK_SERVO_LINREG;
			} else {
				fprintf(stderr,
					"invalid servo name %s\n", optarg);
				return -1;
			}
			break;
		case 'P':
			if (get_arg_val_d(c, optarg, &configured_pi_kp,
					  0.0, DBL_MAX))
				return -1;
			break;
		case 'I':
			if (get_arg_val_d(c, optarg, &configured_pi_ki,
					  0.0, DBL_MAX))
				return -1;
			break;
		case 'S':
			if (get_arg_val_d(c, optarg, &servo_step_threshold,
					  0.0, DBL_MAX))
				return -1;
			break;
		case 'F':
			if (get_arg_val_d(c, optarg, &servo_first_step_threshold,
					  0.0, DBL_MAX))
				return -1;
			break;
		case 'R':
			if (get_arg_val_d(c, optarg, &phc_rate, 1e-9, DBL_MAX))
				return -1;
			node.phc_interval = 1.0 / phc_rate;
			break;
		case 'N':
			if (get_arg_val_i(c, optarg, &node.phc_readings, 1, INT_MAX))
				return -1;
			break;
		case 'O':
			if (get_arg_val_i(c, optarg, &node.sync_offset,
					  INT_MIN, INT_MAX))
				return -1;
			node.forced_sync_offset = -1;
			break;
		case 'L':
			if (get_arg_val_i(c, optarg, &node.sanity_freq_limit, 0, INT_MAX))
				return -1;
			break;
		case 'u':
			if (get_arg_val_ui(c, optarg, &node.stats_max_count,
					  0, UINT_MAX))
				return -1;
			break;
		case 'w':
			wait_sync = 1;
			break;
		case 'n':
			if (get_arg_val_i(c, optarg, &domain_number, 0, 255))
				return -1;
			break;
		case 'x':
			node.kernel_leap = 0;
			break;
		case 'l':
			if (get_arg_val_i(c, optarg, &print_level,
					  PRINT_LEVEL_MIN, PRINT_LEVEL_MAX))
				return -1;
			break;
		case 'm':
			verbose = 1;
			break;
		case 'q':
			use_syslog = 0;
			break;
		case 'v':
			version_show(stdout);
			return 0;
		case 'h':
			usage(progname);
			return 0;
		default:
			goto bad_usage;
		}
	}

	if (pps_fd < 0 && src == CLOCK_INVALID) {
		fprintf(stderr,
			"valid source clock must be selected.\n");
		goto bad_usage;
	}

	if (dst == CLOCK_INVALID) {
		fprintf(stderr,
			"valid destination clock must be selected.\n");
		goto bad_usage;
	}

	if (pps_fd >= 0 && dst != CLOCK_REALTIME) {
		fprintf(stderr,
			"cannot use a pps device unless destination is CLOCK_REALTIME\n");
		goto bad_usage;
	}

	if (!wait_sync && !node.forced_sync_offset) {
		fprintf(stderr,
			"time offset must be specified using -w or -O\n");
		goto bad_usage;
	}

	print_set_progname(progname);
	print_set_verbose(verbose);
	print_set_syslog(use_syslog);
	print_set_level(print_level);

	clock_add(&node, src);
	node.master = LIST_FIRST(&node.clocks);
	clock_add(&node, dst);

	if (wait_sync) {
		if (init_pmc(&node, domain_number))
			return -1;

		while (1) {
			r = run_pmc_wait_sync(&node, 1000);
			if (r < 0)
				return -1;
			if (r > 0)
				break;
			else
				pr_notice("Waiting for ptp4l...");
		}

		if (!node.forced_sync_offset) {
			r = run_pmc_get_utc_offset(&node, 1000);
			if (r <= 0) {
				pr_err("failed to get UTC offset");
				return -1;
			}
		}

		if (node.forced_sync_offset ||
		    (src != CLOCK_REALTIME && dst != CLOCK_REALTIME) ||
		    src == CLOCK_INVALID)
			close_pmc(&node);
	}

	if (pps_fd >= 0) {
		/* only one destination clock allowed with PPS until we
		 * implement a mean to specify PTP port to PPS mapping */
		struct clock *dst_clock;

		LIST_FOREACH(dst_clock, &node.clocks, list) {
			if (dst_clock != node.master)
				break;
		}
		servo_sync_interval(dst_clock->servo, 1.0);
		return do_pps_loop(&node, dst_clock, pps_fd);
	}

	return do_loop(&node);

bad_usage:
	usage(progname);
	return -1;
}
