/*-
 * Public Domain 2014-2018 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wt_internal.h"

/*
 * If we're being asked to sleep a short amount of time, ignore it.
 * A non-zero value means there may be a temporary violation of the
 * capacity limitation, but one that would even out. That is, possibly
 * fewer sleeps with the risk of more choppy behavior as this number
 * is larger.
 */
#define	WT_THROTTLE_SLEEP_CUTOFF_US	0

/*
 * __wt_throttle --
 *	Reserve a time to perform a write operation for the subsystem,
 * and wait until that time.
 *
 * The concept is that each write to a subsystem reserves a time slot
 * to do its write, and atomically adjusts the reservation marker to
 * point past the reserved slot. The size of the adjustment (i.e. the
 * length of time represented by the slot in nanoseconds) is chosen to
 * be proportional to the number of bytes to be written, and the
 * proportion is a simple calculation so that we can fit reservations for
 * exactly the configured capacity in a second. Reservation times are
 * in nanoseconds since the epoch.
 */
int
__wt_throttle(WT_SESSION_IMPL *session, uint64_t bytes, WT_THROTTLE_TYPE type)
{
	struct timespec now;
	WT_CONNECTION_IMPL *conn;
	uint64_t *reservation;
	uint64_t capacity, new_res_len, new_res_value, now_ns, res_len,
	    res_value, sleep_us;

	conn = S2C(session);

	switch (type) {
	case WT_THROTTLE_CKPT:
		capacity = conn->capacity_ckpt;
		reservation = &conn->reservation_ckpt;
		break;
	case WT_THROTTLE_EVICT:
		capacity = conn->capacity_evict;
		reservation = &conn->reservation_evict;
		break;
	case WT_THROTTLE_LOG:
		capacity = conn->capacity_log;
		reservation = &conn->reservation_log;
		break;
	WT_ILLEGAL_VALUE(session, type);
	}

	if (capacity == 0)
		return (0);

	/* Sizes larger than this may overflow */
	WT_ASSERT(session, bytes < 16 * (uint64_t)WT_GIGABYTE);
	res_len = (bytes * WT_BILLION) / capacity;
	res_value = __wt_atomic_add64(reservation, res_len);
	__wt_epoch(session, &now);

	/* Convert the current time to nanoseconds since the epoch. */
	now_ns = (uint64_t)now.tv_sec * WT_BILLION + (uint64_t)now.tv_nsec;

	/*
	 * If the reservation time we got is far enough in the future, see if
	 * stealing a reservation from the checkpoint subsystem makes sense.
	 * This is allowable if there is not currently a checkpoint and
	 * the checkpoint system is configured to have a capacity.
	 */
	if (res_value > now_ns && res_value - now_ns > 100000 &&
	    type != WT_THROTTLE_LOG && !conn->txn_global.checkpoint_running &&
	    conn->capacity_ckpt != 0) {
		new_res_len = (bytes * WT_BILLION) / conn->capacity_ckpt;
		new_res_value = __wt_atomic_add64(
		    &conn->reservation_ckpt, new_res_len);

		/*
		 * If the checkpoint reservation is a better deal (that is,
		 * if we'll sleep for less time), shuffle values so it is
		 * used instead. In either case, we 'return' the reservation
		 * that we aren't using.
		 */
		if (new_res_value < res_value) {
			res_value = new_res_value;
			reservation = &conn->reservation_ckpt;
			capacity = conn->capacity_ckpt;
			res_value = __wt_atomic_sub64(reservation, res_len);
		} else
			(void)__wt_atomic_sub64(
			    &conn->reservation_ckpt, new_res_len);
	}
	if (res_value > now_ns) {
		sleep_us = (res_value - now_ns) / WT_THOUSAND;
		if (sleep_us > WT_THROTTLE_SLEEP_CUTOFF_US) {
			__wt_sleep(sleep_us / WT_MILLION,
			    sleep_us % WT_MILLION);
		}
	/*
	 * If it looks like the reservation clock is out of date by more than
	 * a second, bump it up within a second of the current time. Basically
	 * we don't allow a lot of current bandwidth to 'make up' for
	 * long lulls in the past.
	 *
	 * XXX  We may want to tune this, depending on how we want to treat
	 * bursts of write traffic.
	 */
	} else if (now_ns - res_value > capacity)
		__wt_atomic_store64(reservation, now_ns - capacity + res_value);

	return (0);
}
