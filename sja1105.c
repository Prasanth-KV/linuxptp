/**
 * @file sja1105.c
 * @brief definiton for SJA1105 sync functions
 * @note Copyright 2017 NXP
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
#include <sja1105/ptp.h>
#include <sja1105/staging-area.h>
#include <sja1105/static-config.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "missing.h"
#include "print.h"
#include "sja1105.h"

int SJA1105_VERBOSE_CONDITION = 1;
int SJA1105_DEBUG_CONDITION = 1;

struct sja1105_spi_setup spi_setup = {
	.device    = "/dev/spidev0.1",
	.mode      = SPI_CPHA,
	.bits      = 8,
	.speed     = 10000000,
	.delay     = 0,
	.cs_change = 0,
	.fd        = -1,
};

struct sja1105_sync_timer    sja1105_sync_t;

int sja1105_sync_timer_is_valid()
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;
	return t->valid;
}

int sja1105_parse_staging_area(char *filename)
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;
	struct sja1105_staging_area staging_area;
	unsigned int staging_area_len;
	struct stat stat;
	uint64_t delta;
	int    i, fd;
	char  *buf;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		pr_err("Cannot open staging area at %s!", filename);
		return -1;
	}
	if (fstat(fd, &stat) < 0) {
		pr_err("could not read staging area file size");
		return -1;
	}
	staging_area_len = stat.st_size;
	buf = (char*) malloc(staging_area_len * sizeof(char));
	if (!buf) {
		pr_err("malloc failed");
		return -1;
	}
	if (read(fd, buf, staging_area_len) < 0) {
		pr_err("read failed from staging area");
		return -1;
	}
	/* Static config */
	if (sja1105_static_config_unpack(buf, &staging_area.static_config) < 0) {
		pr_err("error while interpreting config");
		return -1;
	}

	if (staging_area.static_config.schedule_entry_points_params_count > 0 &&
	    staging_area.static_config.schedule_entry_points_params[0].clksrc == 3) {
		/* Qbv is enabled, and clock source is PTP */
		pr_debug("SJA1105 configuration has Qbv enabled.");
		t->have_qbv = 1;
		delta = 0;
		for (i = 0; i < staging_area.static_config.schedule_count; i++) {
			pr_debug("timeslot %i: delta %llu", i,
			         staging_area.static_config.schedule[i].delta);
			delta += staging_area.static_config.schedule[i].delta;
		}
		t->qbv_cycle_len.tv_sec  = (delta * 200) / NS_PER_SEC;
		t->qbv_cycle_len.tv_nsec = (delta * 200) % NS_PER_SEC;
		pr_debug("Qbv cycle duration is [%ld.%09ld]",
		         t->qbv_cycle_len.tv_sec,
		         t->qbv_cycle_len.tv_nsec);
	} else
		t->have_qbv = 0;
	close(fd);
	return 0;
}

/* Initialize the global struct sja1105_sync_t and
 * struct sja1105_sync_pi_s.
 */
int sja1105_sync_timer_create(struct config *config)
{
	struct sja1105_sync_timer    *t = &sja1105_sync_t;
	struct sja1105_sync_pi_servo *s = &t->sync_pi_s;

	t->max_offset = config_get_int(config, NULL, "sja1105_max_offset");
	if (!t->max_offset) {
		pr_debug("sja1105: don't create timer for sync");
		return -1;
	}

	t->max_offset *= 1000;

	pr_debug("sja1105: initialize sja1105 and create timer for sync");

	if (sja1105_spi_configure(&spi_setup) < 0) {
		pr_err("spi_configure failed");
		return -1;
	}

	t->fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (t->fd < 0) {
		pr_err("sja1105: failed to create timer for sync");
		return -1;
	}

	t->valid = 1;
	/* This will set t->ratio to 1 later in sja1105_sync() */
	t->reset_req = 1;

	s->kp = config_get_double(config, NULL, "sja1105_sync_kp");
	s->ki = config_get_double(config, NULL, "sja1105_sync_ki");

	if (sja1105_parse_staging_area("/lib/firmware/sja1105.bin") < 0) {
		pr_err("Parsing staging area failed");
		return -1;
	}

	return 0;
}

void sja1105_sync_fill_pollfd(struct pollfd *dest)
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;

	dest[0].fd = t->fd;
	dest[0].events = POLLIN|POLLPRI;
}

int sja1105_sync_timer_settime(void)
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;
	/* Sync 8 times every second */
	struct itimerspec tmo = {
		{0, 0}, {0, 125000000}
	};

	if (timerfd_settime(t->fd, 0, &tmo, NULL)) {
		pr_err("sja1105: failed to set sync timer");
		t->valid = 0;
		close(t->fd);
		return -1;
	}
	return 0;
}

/* Calculate delay and offset between
 * clkid and SJA1105 PTP clock
 */
static int sja1105_calculate(clockid_t clkid, int64_t *delay, int64_t *offset)
{
	struct timespec t1_spec, t2_spec, t3_spec;
	int gettings;
	int64_t interval, best_interval = INT64_MAX;
	int rc1, rc2, rc3;

	memset(&t1_spec, 0, sizeof(t1_spec));
	memset(&t3_spec, 0, sizeof(t3_spec));

	/* Pick the best interval */
	for (gettings = 0; gettings < 3; gettings ++) {
		if ((rc1 = clock_gettime(clkid, &t1_spec)) ||
		    (rc2 = sja1105_ptp_clk_get(&spi_setup, &t2_spec)) < 0 ||
		    (rc3 = clock_gettime(clkid, &t3_spec))) {
			pr_err("rc1 %d rc2 %d rc3 %d", rc1, rc2, rc3);
			pr_err("sja1105: calculating got time error");
			return -1;
		}

		interval = (t3_spec.tv_sec - t1_spec.tv_sec) * NS_PER_SEC +
		           (t3_spec.tv_nsec - t1_spec.tv_nsec);
		if (interval < best_interval) {
			best_interval = interval;
			*offset = t2_spec.tv_sec * NS_PER_SEC + t2_spec.tv_nsec -
			         (t1_spec.tv_sec * NS_PER_SEC + t1_spec.tv_nsec) -
			          interval / 2;
		}
	}
	*delay = best_interval / 2;

	return 0;
}

#define ADJ_SCALE	10000000

/* Calculate clkrate adjustment based on offset.
 * Function is stateful, keeping history in the global
 * struct sja1105_sync_pi_s.
 **/
static double sja1105_sync_run_pi_servo(int64_t offset)
{
	struct sja1105_sync_pi_servo *s = &sja1105_sync_t.sync_pi_s;
	int64_t adj;

	s->drift_sum += offset * s->ki;
	if (s->drift_sum > ADJ_SCALE)
		s->drift_sum = ADJ_SCALE;
	if (s->drift_sum < - ADJ_SCALE)
		s->drift_sum = - ADJ_SCALE;

	adj = offset * s->kp + s->drift_sum;
	adj = - adj;

	return (double)adj / (double)ADJ_SCALE;
}

int timespec_lower(const struct timespec *lhs, const struct timespec *rhs)
{
	if (lhs->tv_sec == rhs->tv_sec)
		return lhs->tv_nsec < rhs->tv_nsec;
	else
		return lhs->tv_sec < rhs->tv_sec;
}

void timespec_diff(const struct timespec *start, const struct timespec *stop,
                   struct timespec *result)
{
	if ((stop->tv_nsec - start->tv_nsec) < 0) {
		result->tv_sec = stop->tv_sec - start->tv_sec - 1;
		result->tv_nsec = stop->tv_nsec - start->tv_nsec + NS_PER_SEC;
	} else {
		result->tv_sec = stop->tv_sec - start->tv_sec;
		result->tv_nsec = stop->tv_nsec - start->tv_nsec;
	}
}

int sja1105_qbv_stop()
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;
	int rc;

	if (t->have_qbv == 0)
		return -1;
	if (t->qbv_state == QBV_STATE_DISABLED) {
		pr_debug("sja1105: qbv disabled, no need to stop");
		return 0;
	}
	rc = sja1105_ptp_qbv_stop(&spi_setup);
	if (rc < 0) {
		pr_err("sja1105_ptp_qbv_stop failed");
		return -1;
	}
	return 0;
}

int sja1105_qbv_start()
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;
	struct timespec ptpclk_now;
	uint64_t ptpclk_now_ns;
	uint64_t qbv_cycle_len_ns;
	uint64_t qbv_start_time_ns;
	int rc;

	if (t->have_qbv == 0)
		return -1;
	rc = sja1105_ptp_clk_get(&spi_setup, &ptpclk_now);
	if (rc < 0) {
		pr_err("failed to read ptpclk");
		return -1;
	}
	/* Delay start time to the beginning of the first Qbv cycle that starts
	 * at least 3 seconds from now. This should buy us some time.
	 */
	ptpclk_now.tv_sec += 3;
	sja1105_timespec_to_ptp_time(&ptpclk_now, &ptpclk_now_ns);
	sja1105_timespec_to_ptp_time(&t->qbv_cycle_len, &qbv_cycle_len_ns);
	qbv_start_time_ns = (1 + ptpclk_now_ns / qbv_cycle_len_ns) * qbv_cycle_len_ns;
	sja1105_ptp_time_to_timespec(&t->qbv_start_time, qbv_start_time_ns);

	rc = sja1105_ptp_qbv_start_time_set(&spi_setup, &t->qbv_start_time);
	if (rc < 0) {
		pr_err("sja1105_ptp_qbv_start_time_set failed");
		return -1;
	}
	rc = sja1105_ptp_qbv_correction_period_set(&spi_setup,
	                                           &t->qbv_cycle_len);
	if (rc < 0) {
		pr_err("sja1105_ptp_qbv_correction_period_set failed");
		return -1;
	}
	rc = sja1105_ptp_qbv_start(&spi_setup);
	if (rc < 0) {
		pr_err("sja1105_ptp_qbv_start failed");
		return -1;
	}
	return 0;
}

int sja1105_qbv_monitor(int64_t delay, int64_t offset)
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;
	struct timespec ptpclk;
	struct timespec diff;

	if (t->have_qbv == 0)
		return -1;
	pr_debug("sja1105 ratio: %lf", t->ratio);

	switch (t->qbv_state) {
	case QBV_STATE_DISABLED:
		pr_debug("%s: state disabled", __func__);
		if (t->reset_req)
			break;
		if (offset <= -(t->max_offset / 2) ||
		    offset >=   t->max_offset / 2)
			break;
		/* Offset is good enough, start Qbv engine */
		if (sja1105_qbv_start() < 0) {
			pr_err("sja1105_qbv_start failed");
			return -1;
		}
		t->qbv_state = QBV_STATE_ENABLED_NOT_RUNNING;
		break;
	case QBV_STATE_ENABLED_NOT_RUNNING:
		/* Check if Qbv engine has actually started, by
		 * comparing the scheduled start time with the
		 * SJA1105 PTP clock
		 */
		pr_debug("%s: state enabled not running", __func__);
		if (t->reset_req) {
			sja1105_qbv_stop();
			t->qbv_state = QBV_STATE_DISABLED;
			break;
		}
		if (sja1105_ptp_clk_get(&spi_setup, &ptpclk) < 0) {
			pr_err("failed to read ptpclk");
			return -1;
		}
		if (timespec_lower(&ptpclk, &t->qbv_start_time)) {
			/* Qbv has not started yet */
			timespec_diff(&ptpclk, &t->qbv_start_time, &diff);
			pr_debug("time to start: [%ld.%09ld]",
			         diff.tv_sec, diff.tv_nsec);
		} else {
			/* Time elapsed, what happened? */
			if (sja1105_ptp_qbv_running(&spi_setup) == 0) {
				/* Qbv has started */
				t->qbv_state = QBV_STATE_RUNNING;
				pr_debug("%s: transitioned to running state",
				         __func__);
			} else {
				t->qbv_state = QBV_STATE_DISABLED;
				pr_err("%s: not started despite time elapsed",
				       __func__);
			}
		}
		break;
	case QBV_STATE_RUNNING:
		pr_debug("%s: state running", __func__);
		if (t->reset_req) {
			sja1105_qbv_stop();
			t->qbv_state = QBV_STATE_DISABLED;
			break;
		}
		if (sja1105_ptp_qbv_running(&spi_setup) != 0) {
			pr_debug("%s: surprisingly stopped", __func__);
			t->qbv_state = QBV_STATE_DISABLED;
			break;
		}
		if (sja1105_ptp_clk_get(&spi_setup, &ptpclk) < 0) {
			pr_err("failed to read ptpclk");
			return -1;
		}
		timespec_diff(&t->qbv_start_time, &ptpclk, &diff);
		pr_debug("time since started: [%ld.%09ld]",
		         diff.tv_sec, diff.tv_nsec);
		break;
	}
	return 0;
}

int sja1105_sync(clockid_t clkid)
{
	struct sja1105_sync_timer *t = &sja1105_sync_t;
	struct sja1105_sync_pi_servo *s = &t->sync_pi_s;
	struct timespec cur_t;
	int64_t delay, offset;
	struct timespec offset_ts;

	if (t->reset_req) {
		pr_err("sja1105 reset requested");
		/* Step 0, reset sja1105 switch */
		if (sja1105_ptp_reset(&spi_setup)) {
			pr_err("sja1105: reset failed");
			return -1;
		}
		/* Step 1, reset sja1105 ratio */
		t->ratio = 1.0f;
		if (sja1105_ptp_clk_rate_set(&spi_setup, t->ratio)) {
			pr_err("sja1105: set_clock_ratio failed");
			return -1;
		}
		/* Step 2, set sja1105 time prior to master about 1s.
		 * This makes we can set PTPCLKADD register with offset later.
		 */
		if (clock_gettime(clkid, &cur_t)) {
			pr_err("sja1105: clock_gettime error");
			return -1;
		}
		cur_t.tv_sec -= 1;
		if (sja1105_ptp_clk_set(&spi_setup, &cur_t) < 0) {
			pr_err("sja1105_ptp_clk_set failed");
			return -1;
		}
		/* Step 3, calculate delay and offset */
		if (sja1105_calculate(clkid, &delay, &offset))
			return -1;
		/* Step 4, set offset into PTPCLKADD */
		if (offset > 0)
			return -1;
		offset_ts.tv_sec  = (-offset) / NS_PER_SEC;
		offset_ts.tv_nsec = (-offset) % NS_PER_SEC;
		if (sja1105_ptp_clk_add(&spi_setup, &offset_ts) < 0) {
			pr_err("sja1105_ptp_clk_add failed");
			return -1;
		}
		s->drift_sum = 0;
	}

	if (sja1105_calculate(clkid, &delay, &offset))
		return -1;

	pr_debug("sja1105: offset %9lld ns, delay %9lld ns", offset, delay);

	if (offset >= t->max_offset || offset <= - t->max_offset) {
		pr_err("sja1105: offset from master exceeded max value %lld ns",
		       t->max_offset);

		if (offset >= NS_PER_SEC || offset <= - NS_PER_SEC)
			t->reset_req = 1;
		return 0;
	}

	/* Apply adjustment to the SJA1105 clock ratio
	 * according to the PI algorithm */
	t->ratio = 1 + sja1105_sync_run_pi_servo(offset);
	if (sja1105_ptp_clk_rate_set(&spi_setup, t->ratio)) {
		pr_err("sja1105: set_clock_ratio failed");
		return -1;
	}

	sja1105_qbv_monitor(delay, offset);
	t->reset_req = 0;
	return 0;
}

void sja1105_sync_timer_destroy()
{
	sja1105_qbv_stop();
}
