/*
 * Copyright © 2010 Intel Corporation
 * Copyright © 2013 Jonas Ådahl
 * Copyright © 2013-2017 Red Hat, Inc.
 * Copyright © 2017 James Ye <jye836@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <mtdev-plumbing.h>

#include "evdev.h"

#define	DEBOUNCE_TIME ms2us(12)

struct fallback_dispatch {
	struct evdev_dispatch base;
	struct evdev_device *device;

	struct libinput_device_config_calibration calibration;

	struct {
		bool is_enabled;
		int angle;
		struct matrix matrix;
		struct libinput_device_config_rotation config;
	} rotation;

	struct {
		struct device_coords point;
		int32_t seat_slot;

		struct {
			struct device_coords min, max;
			struct ratelimit range_warn_limit;
		} warning_range;
	} abs;

	struct {
		int slot;
		struct mt_slot *slots;
		size_t slots_len;
		bool want_hysteresis;
		struct device_coords hysteresis_margin;
	} mt;

	struct device_coords rel;

	struct {
		/* The struct for the tablet mode switch device itself */
		struct {
			int state;
		} sw;
		/* The struct for other devices listening to the tablet mode
		   switch */
		struct {
			struct evdev_device *sw_device;
			struct libinput_event_listener listener;
		} other;
	} tablet_mode;

	/* Bitmask of pressed keys used to ignore initial release events from
	 * the kernel. */
	unsigned long hw_key_mask[NLONGS(KEY_CNT)];

	enum evdev_event_type pending_event;

	/* true if we're reading events (i.e. not suspended) but we're
	   ignoring them */
	bool ignore_events;

	struct {
		enum evdev_debounce_state state;
		unsigned int button_code;
		uint64_t button_up_time;
		struct libinput_timer timer;
	} debounce;

	struct {
		enum switch_reliability reliability;

		bool is_closed;
		bool is_closed_client_state;

		/* We allow up to 3 paired keyboards for the lid switch
		 * listener. Only one keyboard should exist, but that can
		 * have more than one event node.
		 *
		 * Note: this is a sparse list, any element may have a
		 * non-NULL device.
		 */
		struct paired_keyboard {
			struct evdev_device *device;
			struct libinput_event_listener listener;
		} paired_keyboard[3];
	} lid;
};

static inline struct fallback_dispatch*
fallback_dispatch(struct evdev_dispatch *dispatch)
{
	evdev_verify_dispatch_type(dispatch, DISPATCH_FALLBACK);

	return container_of(dispatch, struct fallback_dispatch, base);
}
enum key_type {
	KEY_TYPE_NONE,
	KEY_TYPE_KEY,
	KEY_TYPE_BUTTON,
};

static void
hw_set_key_down(struct fallback_dispatch *dispatch, int code, int pressed)
{
	long_set_bit_state(dispatch->hw_key_mask, code, pressed);
}

static bool
hw_is_key_down(struct fallback_dispatch *dispatch, int code)
{
	return long_bit_is_set(dispatch->hw_key_mask, code);
}

static int
get_key_down_count(struct evdev_device *device, int code)
{
	return device->key_count[code];
}

static void
fallback_keyboard_notify_key(struct fallback_dispatch *dispatch,
			     struct evdev_device *device,
			     uint64_t time,
			     int key,
			     enum libinput_key_state state)
{
	int down_count;

	down_count = evdev_update_key_down_count(device, key, state);

	if ((state == LIBINPUT_KEY_STATE_PRESSED && down_count == 1) ||
	    (state == LIBINPUT_KEY_STATE_RELEASED && down_count == 0))
		keyboard_notify_key(&device->base, time, key, state);
}

static void
fallback_lid_notify_toggle(struct fallback_dispatch *dispatch,
			   struct evdev_device *device,
			   uint64_t time)
{
	if (dispatch->lid.is_closed ^ dispatch->lid.is_closed_client_state) {
		switch_notify_toggle(&device->base,
				     time,
				     LIBINPUT_SWITCH_LID,
				     dispatch->lid.is_closed);
		dispatch->lid.is_closed_client_state = dispatch->lid.is_closed;
	}
}

static enum libinput_switch_state
fallback_interface_get_switch_state(struct evdev_dispatch *evdev_dispatch,
				    enum libinput_switch sw)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(evdev_dispatch);

	switch (sw) {
	case LIBINPUT_SWITCH_TABLET_MODE:
		break;
	default:
		/* Internal function only, so we can abort here */
		abort();
	}

	return dispatch->tablet_mode.sw.state ?
			LIBINPUT_SWITCH_STATE_ON :
			LIBINPUT_SWITCH_STATE_OFF;
}

static inline void
normalize_delta(struct evdev_device *device,
		const struct device_coords *delta,
		struct normalized_coords *normalized)
{
	normalized->x = delta->x * DEFAULT_MOUSE_DPI / (double)device->dpi;
	normalized->y = delta->y * DEFAULT_MOUSE_DPI / (double)device->dpi;
}

static inline bool
post_trackpoint_scroll(struct evdev_device *device,
		       struct normalized_coords unaccel,
		       uint64_t time)
{
	if (device->scroll.method != LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN)
		return false;

	switch(device->scroll.button_scroll_state) {
	case BUTTONSCROLL_IDLE:
		return false;
	case BUTTONSCROLL_BUTTON_DOWN:
		/* if the button is down but scroll is not active, we're within the
		   timeout where swallow motion events but don't post scroll buttons */
		evdev_log_debug(device, "btnscroll: discarding\n");
		return true;
	case BUTTONSCROLL_READY:
		device->scroll.button_scroll_state = BUTTONSCROLL_SCROLLING;
		/* fallthrough */
	case BUTTONSCROLL_SCROLLING:
		evdev_post_scroll(device, time,
				  LIBINPUT_POINTER_AXIS_SOURCE_CONTINUOUS,
				  &unaccel);
		return true;
	}

	assert(!"invalid scroll button state");
}

static inline bool
fallback_filter_defuzz_touch(struct fallback_dispatch *dispatch,
			     struct evdev_device *device,
			     struct mt_slot *slot)
{
	struct device_coords point;

	if (!dispatch->mt.want_hysteresis)
		return false;

	point.x = evdev_hysteresis(slot->point.x,
				   slot->hysteresis_center.x,
				   dispatch->mt.hysteresis_margin.x);
	point.y = evdev_hysteresis(slot->point.y,
				   slot->hysteresis_center.y,
				   dispatch->mt.hysteresis_margin.y);

	slot->hysteresis_center = slot->point;
	if (point.x == slot->point.x && point.y == slot->point.y)
		return true;

	slot->point = point;

	return false;
}

static inline void
fallback_rotate_relative(struct fallback_dispatch *dispatch,
			 struct evdev_device *device)
{
	struct device_coords rel = dispatch->rel;

	if (!device->base.config.rotation)
		return;

	/* loss of precision for non-90 degrees, but we only support 90 deg
	 * right now anyway */
	matrix_mult_vec(&dispatch->rotation.matrix, &rel.x, &rel.y);

	dispatch->rel = rel;
}

static void
fallback_flush_relative_motion(struct fallback_dispatch *dispatch,
			       struct evdev_device *device,
			       uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct normalized_coords accel, unaccel;
	struct device_float_coords raw;

	if (!(device->seat_caps & EVDEV_DEVICE_POINTER))
		return;

	fallback_rotate_relative(dispatch, device);

	normalize_delta(device, &dispatch->rel, &unaccel);
	raw.x = dispatch->rel.x;
	raw.y = dispatch->rel.y;
	dispatch->rel.x = 0;
	dispatch->rel.y = 0;

	/* Use unaccelerated deltas for pointing stick scroll */
	if (post_trackpoint_scroll(device, unaccel, time))
		return;

	if (device->pointer.filter) {
		/* Apply pointer acceleration. */
		accel = filter_dispatch(device->pointer.filter,
					&raw,
					device,
					time);
	} else {
		evdev_log_bug_libinput(device,
				       "accel filter missing\n");
		accel = unaccel;
	}

	if (normalized_is_zero(accel) && normalized_is_zero(unaccel))
		return;

	pointer_notify_motion(base, time, &accel, &raw);
}

static void
fallback_flush_absolute_motion(struct fallback_dispatch *dispatch,
			       struct evdev_device *device,
			       uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct device_coords point;

	if (!(device->seat_caps & EVDEV_DEVICE_POINTER))
		return;

	point = dispatch->abs.point;
	evdev_transform_absolute(device, &point);

	pointer_notify_motion_absolute(base, time, &point);
}

static bool
fallback_flush_mt_down(struct fallback_dispatch *dispatch,
		       struct evdev_device *device,
		       int slot_idx,
		       uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct libinput_seat *seat = base->seat;
	struct device_coords point;
	struct mt_slot *slot;
	int seat_slot;

	if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
		return false;

	slot = &dispatch->mt.slots[slot_idx];
	if (slot->seat_slot != -1) {
		evdev_log_bug_kernel(device,
				     "driver sent multiple touch down for the same slot");
		return false;
	}

	seat_slot = ffs(~seat->slot_map) - 1;
	slot->seat_slot = seat_slot;

	if (seat_slot == -1)
		return false;

	seat->slot_map |= 1 << seat_slot;
	point = slot->point;
	slot->hysteresis_center = point;
	evdev_transform_absolute(device, &point);

	touch_notify_touch_down(base, time, slot_idx, seat_slot,
				&point);

	return true;
}

static bool
fallback_flush_mt_motion(struct fallback_dispatch *dispatch,
			 struct evdev_device *device,
			 int slot_idx,
			 uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct device_coords point;
	struct mt_slot *slot;
	int seat_slot;

	if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
		return false;

	slot = &dispatch->mt.slots[slot_idx];
	seat_slot = slot->seat_slot;
	point = slot->point;

	if (seat_slot == -1)
		return false;

	if (fallback_filter_defuzz_touch(dispatch, device, slot))
		return false;

	evdev_transform_absolute(device, &point);
	touch_notify_touch_motion(base, time, slot_idx, seat_slot,
				  &point);

	return true;
}

static bool
fallback_flush_mt_up(struct fallback_dispatch *dispatch,
		     struct evdev_device *device,
		     int slot_idx,
		     uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct libinput_seat *seat = base->seat;
	struct mt_slot *slot;
	int seat_slot;

	if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
		return false;

	slot = &dispatch->mt.slots[slot_idx];
	seat_slot = slot->seat_slot;
	slot->seat_slot = -1;

	if (seat_slot == -1)
		return false;

	seat->slot_map &= ~(1 << seat_slot);

	touch_notify_touch_up(base, time, slot_idx, seat_slot);

	return true;
}

static bool
fallback_flush_st_down(struct fallback_dispatch *dispatch,
		       struct evdev_device *device,
		       uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct libinput_seat *seat = base->seat;
	struct device_coords point;
	int seat_slot;

	if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
		return false;

	if (dispatch->abs.seat_slot != -1) {
		evdev_log_bug_kernel(device,
				     "driver sent multiple touch down for the same slot");
		return false;
	}

	seat_slot = ffs(~seat->slot_map) - 1;
	dispatch->abs.seat_slot = seat_slot;

	if (seat_slot == -1)
		return false;

	seat->slot_map |= 1 << seat_slot;

	point = dispatch->abs.point;
	evdev_transform_absolute(device, &point);

	touch_notify_touch_down(base, time, -1, seat_slot, &point);

	return true;
}

static bool
fallback_flush_st_motion(struct fallback_dispatch *dispatch,
			 struct evdev_device *device,
			 uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct device_coords point;
	int seat_slot;

	point = dispatch->abs.point;
	evdev_transform_absolute(device, &point);

	seat_slot = dispatch->abs.seat_slot;

	if (seat_slot == -1)
		return false;

	touch_notify_touch_motion(base, time, -1, seat_slot, &point);

	return true;
}

static bool
fallback_flush_st_up(struct fallback_dispatch *dispatch,
		     struct evdev_device *device,
		     uint64_t time)
{
	struct libinput_device *base = &device->base;
	struct libinput_seat *seat = base->seat;
	int seat_slot;

	if (!(device->seat_caps & EVDEV_DEVICE_TOUCH))
		return false;

	seat_slot = dispatch->abs.seat_slot;
	dispatch->abs.seat_slot = -1;

	if (seat_slot == -1)
		return false;

	seat->slot_map &= ~(1 << seat_slot);

	touch_notify_touch_up(base, time, -1, seat_slot);

	return true;
}

static enum evdev_event_type
fallback_flush_pending_event(struct fallback_dispatch *dispatch,
			     struct evdev_device *device,
			     uint64_t time)
{
	enum evdev_event_type sent_event;
	int slot_idx;

	sent_event = dispatch->pending_event;

	switch (dispatch->pending_event) {
	case EVDEV_NONE:
		break;
	case EVDEV_RELATIVE_MOTION:
		fallback_flush_relative_motion(dispatch, device, time);
		break;
	case EVDEV_ABSOLUTE_MT_DOWN:
		slot_idx = dispatch->mt.slot;
		if (!fallback_flush_mt_down(dispatch,
					    device,
					    slot_idx,
					    time))
			sent_event = EVDEV_NONE;
		break;
	case EVDEV_ABSOLUTE_MT_MOTION:
		slot_idx = dispatch->mt.slot;
		if (!fallback_flush_mt_motion(dispatch,
					      device,
					      slot_idx,
					      time))
			sent_event = EVDEV_NONE;
		break;
	case EVDEV_ABSOLUTE_MT_UP:
		slot_idx = dispatch->mt.slot;
		if (!fallback_flush_mt_up(dispatch,
					  device,
					  slot_idx,
					  time))
			sent_event = EVDEV_NONE;
		break;
	case EVDEV_ABSOLUTE_TOUCH_DOWN:
		if (!fallback_flush_st_down(dispatch, device, time))
			sent_event = EVDEV_NONE;
		break;
	case EVDEV_ABSOLUTE_MOTION:
		if (device->seat_caps & EVDEV_DEVICE_TOUCH) {
			if (fallback_flush_st_motion(dispatch,
						     device,
						     time))
				sent_event = EVDEV_ABSOLUTE_MT_MOTION;
			else
				sent_event = EVDEV_NONE;
		} else if (device->seat_caps & EVDEV_DEVICE_POINTER) {
			fallback_flush_absolute_motion(dispatch,
						       device,
						       time);
		}
		break;
	case EVDEV_ABSOLUTE_TOUCH_UP:
		if (!fallback_flush_st_up(dispatch, device, time))
			sent_event = EVDEV_NONE;
		break;
	default:
		assert(0 && "Unknown pending event type");
		break;
	}

	dispatch->pending_event = EVDEV_NONE;

	return sent_event;
}

static enum key_type
get_key_type(uint16_t code)
{
	switch (code) {
	case BTN_TOOL_PEN:
	case BTN_TOOL_RUBBER:
	case BTN_TOOL_BRUSH:
	case BTN_TOOL_PENCIL:
	case BTN_TOOL_AIRBRUSH:
	case BTN_TOOL_MOUSE:
	case BTN_TOOL_LENS:
	case BTN_TOOL_QUINTTAP:
	case BTN_TOOL_DOUBLETAP:
	case BTN_TOOL_TRIPLETAP:
	case BTN_TOOL_QUADTAP:
	case BTN_TOOL_FINGER:
	case BTN_TOUCH:
		return KEY_TYPE_NONE;
	}

	if (code >= KEY_ESC && code <= KEY_MICMUTE)
		return KEY_TYPE_KEY;
	if (code >= BTN_MISC && code <= BTN_GEAR_UP)
		return KEY_TYPE_BUTTON;
	if (code >= KEY_OK && code <= KEY_LIGHTS_TOGGLE)
		return KEY_TYPE_KEY;
	if (code >= BTN_DPAD_UP && code <= BTN_DPAD_RIGHT)
		return KEY_TYPE_BUTTON;
	if (code >= KEY_ALS_TOGGLE && code <= KEY_ONSCREEN_KEYBOARD)
		return KEY_TYPE_KEY;
	if (code >= BTN_TRIGGER_HAPPY && code <= BTN_TRIGGER_HAPPY40)
		return KEY_TYPE_BUTTON;
	return KEY_TYPE_NONE;
}

static void
fallback_process_touch_button(struct fallback_dispatch *dispatch,
			      struct evdev_device *device,
			      uint64_t time, int value)
{
	if (dispatch->pending_event != EVDEV_NONE &&
	    dispatch->pending_event != EVDEV_ABSOLUTE_MOTION)
		fallback_flush_pending_event(dispatch, device, time);

	dispatch->pending_event = (value ?
				 EVDEV_ABSOLUTE_TOUCH_DOWN :
				 EVDEV_ABSOLUTE_TOUCH_UP);
}

static inline void
fallback_flush_debounce(struct fallback_dispatch *dispatch,
			struct evdev_device *device)
{
	int code = dispatch->debounce.button_code;
	int button;

	if (dispatch->debounce.state != DEBOUNCE_ACTIVE)
		return;

	if (hw_is_key_down(dispatch, code)) {
		button = evdev_to_left_handed(device, code);
		evdev_pointer_notify_physical_button(device,
						     dispatch->debounce.button_up_time,
						     button,
						     LIBINPUT_BUTTON_STATE_RELEASED);
		hw_set_key_down(dispatch, code, 0);
	}

	dispatch->debounce.state = DEBOUNCE_ON;
}

static void
fallback_debounce_timeout(uint64_t now, void *data)
{
	struct evdev_device *device = data;
	struct fallback_dispatch *dispatch =
		fallback_dispatch(device->dispatch);

	fallback_flush_debounce(dispatch, device);
}

static bool
fallback_filter_debounce_press(struct fallback_dispatch *dispatch,
			       struct evdev_device *device,
			       struct input_event *e,
			       uint64_t time)
{
	bool filter = false;
	uint64_t tdelta;

	/* If other button is pressed while we're holding back the release,
	 * flush the pending release (if any) and continue. We don't handle
	 * this situation, if you have a mouse that needs per-button
	 * debouncing, consider writing to santa for a new mouse.
	 */
	if (e->code != dispatch->debounce.button_code) {
		if (dispatch->debounce.state == DEBOUNCE_ACTIVE) {
			libinput_timer_cancel(&dispatch->debounce.timer);
			fallback_flush_debounce(dispatch, device);
		}
		return false;
	}

	tdelta = time - dispatch->debounce.button_up_time;
	assert((int64_t)tdelta >= 0);

	if (tdelta < DEBOUNCE_TIME) {
		switch (dispatch->debounce.state) {
		case DEBOUNCE_INIT:
			/* This is the first time we debounce, enable proper debouncing
			   from now on but filter this press event */
			filter = true;
			evdev_log_info(device,
				       "Enabling button debouncing, "
				       "see %sbutton_debouncing.html for details\n",
				       HTTP_DOC_LINK);
			dispatch->debounce.state = DEBOUNCE_NEEDED;
			break;
		case DEBOUNCE_NEEDED:
		case DEBOUNCE_ON:
			break;
		/* If a release event is pending and, filter press
		 * events until we flushed the release */
		case DEBOUNCE_ACTIVE:
			filter = true;
			break;
		}
	} else if (dispatch->debounce.state == DEBOUNCE_ACTIVE) {
		/* call libinput_dispatch() more frequently */
		evdev_log_bug_client(device,
				     "Debouncing still active past timeout\n");
	}

	return filter;
}

static bool
fallback_filter_debounce_release(struct fallback_dispatch *dispatch,
				 struct input_event *e,
				 uint64_t time)
{
	bool filter = false;

	dispatch->debounce.button_code = e->code;
	dispatch->debounce.button_up_time = time;

	switch (dispatch->debounce.state) {
	case DEBOUNCE_INIT:
		break;
	case DEBOUNCE_NEEDED:
		filter = true;
		dispatch->debounce.state = DEBOUNCE_ON;
		break;
	case DEBOUNCE_ON:
		libinput_timer_set(&dispatch->debounce.timer,
				   time + DEBOUNCE_TIME);
		filter = true;
		dispatch->debounce.state = DEBOUNCE_ACTIVE;
		break;
	case DEBOUNCE_ACTIVE:
		filter = true;
		break;
	}

	return filter;
}

static bool
fallback_filter_debounce(struct fallback_dispatch *dispatch,
			 struct evdev_device *device,
			 struct input_event *e, uint64_t time)
{
	bool filter = false;

	/* Behavior: we monitor the time deltas between release and press
	 * events. Proper debouncing is disabled on init, but the first
	 * time we see a bouncing press event we enable it.
	 *
	 * The first bounced event is simply discarded, which ends up in the
	 * button being released sooner than it should be. Subsequent button
	 * presses are timer-based and thus released a bit later because we
	 * then wait for a timeout before we post the release event.
	 */
	if (e->value)
		filter = fallback_filter_debounce_press(dispatch, device, e, time);
	else
		filter = fallback_filter_debounce_release(dispatch, e, time);

	return filter;
}

static inline void
fallback_process_key(struct fallback_dispatch *dispatch,
		     struct evdev_device *device,
		     struct input_event *e, uint64_t time)
{
	enum key_type type;

	/* ignore kernel key repeat */
	if (e->value == 2)
		return;

	if (e->code == BTN_TOUCH) {
		if (!device->is_mt)
			fallback_process_touch_button(dispatch,
						      device,
						      time,
						      e->value);
		return;
	}

	fallback_flush_pending_event(dispatch, device, time);

	type = get_key_type(e->code);

	/* Ignore key release events from the kernel for keys that libinput
	 * never got a pressed event for or key presses for keys that we
	 * think are still down */
	switch (type) {
	case KEY_TYPE_NONE:
		break;
	case KEY_TYPE_KEY:
		if ((e->value && hw_is_key_down(dispatch, e->code)) ||
		    (e->value == 0 && !hw_is_key_down(dispatch, e->code)))
			return;
		break;
	case KEY_TYPE_BUTTON:
		if (fallback_filter_debounce(dispatch, device, e, time))
			return;

		if ((e->value && hw_is_key_down(dispatch, e->code)) ||
		    (e->value == 0 && !hw_is_key_down(dispatch, e->code)))
			return;
		break;
	}

	hw_set_key_down(dispatch, e->code, e->value);

	switch (type) {
	case KEY_TYPE_NONE:
		break;
	case KEY_TYPE_KEY:
		fallback_keyboard_notify_key(
			dispatch,
			device,
			time,
			e->code,
			e->value ? LIBINPUT_KEY_STATE_PRESSED :
				   LIBINPUT_KEY_STATE_RELEASED);
		break;
	case KEY_TYPE_BUTTON:
		evdev_pointer_notify_physical_button(
			device,
			time,
			evdev_to_left_handed(device, e->code),
			e->value ? LIBINPUT_BUTTON_STATE_PRESSED :
				   LIBINPUT_BUTTON_STATE_RELEASED);
		break;
	}
}

static void
fallback_process_touch(struct fallback_dispatch *dispatch,
		       struct evdev_device *device,
		       struct input_event *e,
		       uint64_t time)
{
	switch (e->code) {
	case ABS_MT_SLOT:
		if ((size_t)e->value >= dispatch->mt.slots_len) {
			evdev_log_bug_libinput(device,
					 "exceeded slot count (%d of max %zd)\n",
					 e->value,
					 dispatch->mt.slots_len);
			e->value = dispatch->mt.slots_len - 1;
		}
		fallback_flush_pending_event(dispatch, device, time);
		dispatch->mt.slot = e->value;
		break;
	case ABS_MT_TRACKING_ID:
		if (dispatch->pending_event != EVDEV_NONE &&
		    dispatch->pending_event != EVDEV_ABSOLUTE_MT_MOTION)
			fallback_flush_pending_event(dispatch, device, time);
		if (e->value >= 0)
			dispatch->pending_event = EVDEV_ABSOLUTE_MT_DOWN;
		else
			dispatch->pending_event = EVDEV_ABSOLUTE_MT_UP;
		break;
	case ABS_MT_POSITION_X:
		evdev_device_check_abs_axis_range(device, e->code, e->value);
		dispatch->mt.slots[dispatch->mt.slot].point.x = e->value;
		if (dispatch->pending_event == EVDEV_NONE)
			dispatch->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	case ABS_MT_POSITION_Y:
		evdev_device_check_abs_axis_range(device, e->code, e->value);
		dispatch->mt.slots[dispatch->mt.slot].point.y = e->value;
		if (dispatch->pending_event == EVDEV_NONE)
			dispatch->pending_event = EVDEV_ABSOLUTE_MT_MOTION;
		break;
	}
}
static inline void
fallback_process_absolute_motion(struct fallback_dispatch *dispatch,
				 struct evdev_device *device,
				 struct input_event *e)
{
	switch (e->code) {
	case ABS_X:
		evdev_device_check_abs_axis_range(device, e->code, e->value);
		dispatch->abs.point.x = e->value;
		if (dispatch->pending_event == EVDEV_NONE)
			dispatch->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	case ABS_Y:
		evdev_device_check_abs_axis_range(device, e->code, e->value);
		dispatch->abs.point.y = e->value;
		if (dispatch->pending_event == EVDEV_NONE)
			dispatch->pending_event = EVDEV_ABSOLUTE_MOTION;
		break;
	}
}

static void
fallback_lid_keyboard_event(uint64_t time,
			    struct libinput_event *event,
			    void *data)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(data);

	if (!dispatch->lid.is_closed)
		return;

	if (event->type != LIBINPUT_EVENT_KEYBOARD_KEY)
		return;

	if (dispatch->lid.reliability == RELIABILITY_WRITE_OPEN) {
		int fd = libevdev_get_fd(dispatch->device->evdev);
		struct input_event ev[2] = {
			{{ 0, 0 }, EV_SW, SW_LID, 0 },
			{{ 0, 0 }, EV_SYN, SYN_REPORT, 0 },
		};

		(void)write(fd, ev, sizeof(ev));
		/* In case write() fails, we sync the lid state manually
		 * regardless. */
	}

	/* Posting the event here means we preempt the keyboard events that
	 * caused us to wake up, so the lid event is always passed on before
	 * the key event.
	 */
	dispatch->lid.is_closed = false;
	fallback_lid_notify_toggle(dispatch, dispatch->device, time);
}

static void
fallback_lid_toggle_keyboard_listener(struct fallback_dispatch *dispatch,
				      struct paired_keyboard *kbd,
				      bool is_closed)
{
	assert(kbd->device);

	libinput_device_remove_event_listener(&kbd->listener);

	if (is_closed) {
		libinput_device_add_event_listener(
					&kbd->device->base,
					&kbd->listener,
					fallback_lid_keyboard_event,
					dispatch);
	} else {
		libinput_device_init_event_listener(&kbd->listener);
	}
}

static void
fallback_lid_toggle_keyboard_listeners(struct fallback_dispatch *dispatch,
				       bool is_closed)
{
	struct paired_keyboard *kbd;

	ARRAY_FOR_EACH(dispatch->lid.paired_keyboard, kbd) {
		if (!kbd->device)
			continue;

		fallback_lid_toggle_keyboard_listener(dispatch,
						      kbd,
						      is_closed);
	}
}

static inline void
fallback_process_switch(struct fallback_dispatch *dispatch,
			struct evdev_device *device,
			struct input_event *e,
			uint64_t time)
{
	enum libinput_switch_state state;
	bool is_closed;

	switch (e->code) {
	case SW_LID:
		is_closed = !!e->value;

		fallback_lid_toggle_keyboard_listeners(dispatch, is_closed);

		if (dispatch->lid.is_closed == is_closed)
			return;

		dispatch->lid.is_closed = is_closed;
		fallback_lid_notify_toggle(dispatch, device, time);
		break;
	case SW_TABLET_MODE:
		if (dispatch->tablet_mode.sw.state == e->value)
			return;

		dispatch->tablet_mode.sw.state = e->value;
		if (e->value)
			state = LIBINPUT_SWITCH_STATE_ON;
		else
			state = LIBINPUT_SWITCH_STATE_OFF;
		switch_notify_toggle(&device->base,
				     time,
				     LIBINPUT_SWITCH_TABLET_MODE,
				     state);
		break;
	}
}

static inline bool
fallback_reject_relative(struct evdev_device *device,
			 const struct input_event *e,
			 uint64_t time)
{
	if ((e->code == REL_X || e->code == REL_Y) &&
	    (device->seat_caps & EVDEV_DEVICE_POINTER) == 0) {
		evdev_log_bug_libinput_ratelimit(device,
						 &device->nonpointer_rel_limit,
						 "REL_X/Y from a non-pointer device\n");
		return true;
	}

	return false;
}

static inline void
fallback_process_relative(struct fallback_dispatch *dispatch,
			  struct evdev_device *device,
			  struct input_event *e, uint64_t time)
{
	struct normalized_coords wheel_degrees = { 0.0, 0.0 };
	struct discrete_coords discrete = { 0.0, 0.0 };
	enum libinput_pointer_axis_source source;

	if (fallback_reject_relative(device, e, time))
		return;

	switch (e->code) {
	case REL_X:
		if (dispatch->pending_event != EVDEV_RELATIVE_MOTION)
			fallback_flush_pending_event(dispatch, device, time);
		dispatch->rel.x += e->value;
		dispatch->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_Y:
		if (dispatch->pending_event != EVDEV_RELATIVE_MOTION)
			fallback_flush_pending_event(dispatch, device, time);
		dispatch->rel.y += e->value;
		dispatch->pending_event = EVDEV_RELATIVE_MOTION;
		break;
	case REL_WHEEL:
		fallback_flush_pending_event(dispatch, device, time);
		wheel_degrees.y = -1 * e->value *
					device->scroll.wheel_click_angle.x;
		discrete.y = -1 * e->value;

		source = device->scroll.is_tilt.vertical ?
				LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT:
				LIBINPUT_POINTER_AXIS_SOURCE_WHEEL;

		evdev_notify_axis(
			device,
			time,
			AS_MASK(LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL),
			source,
			&wheel_degrees,
			&discrete);
		break;
	case REL_HWHEEL:
		fallback_flush_pending_event(dispatch, device, time);
		wheel_degrees.x = e->value *
					device->scroll.wheel_click_angle.y;
		discrete.x = e->value;

		source = device->scroll.is_tilt.horizontal ?
				LIBINPUT_POINTER_AXIS_SOURCE_WHEEL_TILT:
				LIBINPUT_POINTER_AXIS_SOURCE_WHEEL;

		evdev_notify_axis(
			device,
			time,
			AS_MASK(LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL),
			source,
			&wheel_degrees,
			&discrete);
		break;
	}
}

static inline void
fallback_process_absolute(struct fallback_dispatch *dispatch,
			  struct evdev_device *device,
			  struct input_event *e,
			  uint64_t time)
{
	if (device->is_mt) {
		fallback_process_touch(dispatch, device, e, time);
	} else {
		fallback_process_absolute_motion(dispatch, device, e);
	}
}

static inline bool
fallback_any_button_down(struct fallback_dispatch *dispatch,
		      struct evdev_device *device)
{
	unsigned int button;

	for (button = BTN_LEFT; button < BTN_JOYSTICK; button++) {
		if (libevdev_has_event_code(device->evdev, EV_KEY, button) &&
		    hw_is_key_down(dispatch, button))
			return true;
	}
	return false;
}

static void
fallback_interface_process(struct evdev_dispatch *evdev_dispatch,
			   struct evdev_device *device,
			   struct input_event *event,
			   uint64_t time)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(evdev_dispatch);
	enum evdev_event_type sent;

	if (dispatch->ignore_events)
		return;

	switch (event->type) {
	case EV_REL:
		fallback_process_relative(dispatch, device, event, time);
		break;
	case EV_ABS:
		fallback_process_absolute(dispatch, device, event, time);
		break;
	case EV_KEY:
		fallback_process_key(dispatch, device, event, time);
		break;
	case EV_SW:
		fallback_process_switch(dispatch, device, event, time);
		break;
	case EV_SYN:
		sent = fallback_flush_pending_event(dispatch, device, time);
		switch (sent) {
		case EVDEV_ABSOLUTE_TOUCH_DOWN:
		case EVDEV_ABSOLUTE_TOUCH_UP:
		case EVDEV_ABSOLUTE_MT_DOWN:
		case EVDEV_ABSOLUTE_MT_MOTION:
		case EVDEV_ABSOLUTE_MT_UP:
			touch_notify_frame(&device->base, time);
			break;
		case EVDEV_ABSOLUTE_MOTION:
		case EVDEV_RELATIVE_MOTION:
		case EVDEV_NONE:
			break;
		}
		break;
	}
}

static void
release_touches(struct fallback_dispatch *dispatch,
		struct evdev_device *device,
		uint64_t time)
{
	unsigned int idx;
	bool need_frame = false;

	need_frame = fallback_flush_st_up(dispatch, device, time);

	for (idx = 0; idx < dispatch->mt.slots_len; idx++) {
		struct mt_slot *slot = &dispatch->mt.slots[idx];

		if (slot->seat_slot == -1)
			continue;

		if (fallback_flush_mt_up(dispatch, device, idx, time))
			need_frame = true;
	}

	if (need_frame)
		touch_notify_frame(&device->base, time);
}

static void
release_pressed_keys(struct fallback_dispatch *dispatch,
		     struct evdev_device *device,
		     uint64_t time)
{
	int code;

	for (code = 0; code < KEY_CNT; code++) {
		int count = get_key_down_count(device, code);

		if (count == 0)
			continue;

		if (count > 1) {
			evdev_log_bug_libinput(device,
					       "key %d is down %d times.\n",
					       code,
					       count);
		}

		switch (get_key_type(code)) {
		case KEY_TYPE_NONE:
			break;
		case KEY_TYPE_KEY:
			fallback_keyboard_notify_key(
				dispatch,
				device,
				time,
				code,
				LIBINPUT_KEY_STATE_RELEASED);
			break;
		case KEY_TYPE_BUTTON:
			evdev_pointer_notify_physical_button(
				device,
				time,
				evdev_to_left_handed(device, code),
				LIBINPUT_BUTTON_STATE_RELEASED);
			break;
		}

		count = get_key_down_count(device, code);
		if (count != 0) {
			evdev_log_bug_libinput(device,
					       "releasing key %d failed.\n",
					       code);
			break;
		}
	}
}

static void
fallback_return_to_neutral_state(struct fallback_dispatch *dispatch,
				 struct evdev_device *device)
{
	struct libinput *libinput = evdev_libinput_context(device);
	uint64_t time;

	if ((time = libinput_now(libinput)) == 0)
		return;

	release_touches(dispatch, device, time);
	release_pressed_keys(dispatch, device, time);
	memset(dispatch->hw_key_mask, 0, sizeof(dispatch->hw_key_mask));
}

static void
fallback_interface_suspend(struct evdev_dispatch *evdev_dispatch,
			   struct evdev_device *device)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(evdev_dispatch);

	fallback_return_to_neutral_state(dispatch, device);
}

static void
fallback_interface_remove(struct evdev_dispatch *evdev_dispatch)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(evdev_dispatch);
	struct paired_keyboard *kbd;

	libinput_device_remove_event_listener(&dispatch->tablet_mode.other.listener);

	ARRAY_FOR_EACH(dispatch->lid.paired_keyboard, kbd) {
		if (!kbd->device)
			continue;

		libinput_device_remove_event_listener(&kbd->listener);
	}
}

static void
fallback_interface_sync_initial_state(struct evdev_device *device,
				      struct evdev_dispatch *evdev_dispatch)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(evdev_dispatch);
	uint64_t time = libinput_now(evdev_libinput_context(device));

	if (device->tags & EVDEV_TAG_LID_SWITCH) {
		struct libevdev *evdev = device->evdev;

		dispatch->lid.is_closed = libevdev_get_event_value(evdev,
								   EV_SW,
								   SW_LID);
		dispatch->lid.is_closed_client_state = false;

		/* For the initial state sync, we depend on whether the lid switch
		 * is reliable. If we know it's reliable, we sync as expected.
		 * If we're not sure, we ignore the initial state and only sync on
		 * the first future lid close event. Laptops with a broken switch
		 * that always have the switch in 'on' state thus don't mess up our
		 * touchpad.
		 */
		if (dispatch->lid.is_closed &&
		    dispatch->lid.reliability == RELIABILITY_RELIABLE) {
			fallback_lid_notify_toggle(dispatch, device, time);
		}
	}

	if (dispatch->tablet_mode.sw.state) {
		switch_notify_toggle(&device->base,
				     time,
				     LIBINPUT_SWITCH_TABLET_MODE,
				     LIBINPUT_SWITCH_STATE_ON);
	}
}

static void
fallback_interface_toggle_touch(struct evdev_dispatch *evdev_dispatch,
				struct evdev_device *device,
				bool enable)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(evdev_dispatch);
	bool ignore_events = !enable;

	if (ignore_events == dispatch->ignore_events)
		return;

	if (ignore_events)
		fallback_return_to_neutral_state(dispatch, device);

	dispatch->ignore_events = ignore_events;
}

static void
fallback_interface_destroy(struct evdev_dispatch *evdev_dispatch)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(evdev_dispatch);

	libinput_timer_cancel(&dispatch->debounce.timer);
	libinput_timer_destroy(&dispatch->debounce.timer);
	free(dispatch->mt.slots);
	free(dispatch);
}

static void
fallback_lid_pair_keyboard(struct evdev_device *lid_switch,
			   struct evdev_device *keyboard)
{
	struct fallback_dispatch *dispatch =
		fallback_dispatch(lid_switch->dispatch);
	struct paired_keyboard *kbd;
	bool paired = false;

	if ((keyboard->tags & EVDEV_TAG_KEYBOARD) == 0 ||
	    (lid_switch->tags & EVDEV_TAG_LID_SWITCH) == 0)
		return;

	if ((keyboard->tags & EVDEV_TAG_INTERNAL_KEYBOARD) == 0)
		return;

	ARRAY_FOR_EACH(dispatch->lid.paired_keyboard, kbd) {
		if (kbd->device)
			continue;

		kbd->device = keyboard;
		evdev_log_debug(lid_switch,
				"lid: keyboard paired with %s<->%s\n",
				lid_switch->devname,
				keyboard->devname);

		/* We need to init the event listener now only if the
		 * reported state is closed. */
		if (dispatch->lid.is_closed)
			fallback_lid_toggle_keyboard_listener(
					      dispatch,
					      kbd,
					      dispatch->lid.is_closed);
		paired = true;
		break;
	}

	if (!paired)
		evdev_log_bug_libinput(lid_switch,
				       "lid: too many internal keyboards\n");
}

static void
fallback_resume(struct fallback_dispatch *dispatch,
		struct evdev_device *device)
{
	if (dispatch->base.sendevents.current_mode ==
	    LIBINPUT_CONFIG_SEND_EVENTS_DISABLED)
		return;

	evdev_device_resume(device);
}

static void
fallback_suspend(struct fallback_dispatch *dispatch,
		 struct evdev_device *device)
{
	evdev_device_suspend(device);
}

static void
fallback_tablet_mode_switch_event(uint64_t time,
				  struct libinput_event *event,
				  void *data)
{
	struct fallback_dispatch *dispatch = data;
	struct evdev_device *device = dispatch->device;
	struct libinput_event_switch *swev;

	if (libinput_event_get_type(event) != LIBINPUT_EVENT_SWITCH_TOGGLE)
		return;

	swev = libinput_event_get_switch_event(event);
	if (libinput_event_switch_get_switch(swev) !=
	    LIBINPUT_SWITCH_TABLET_MODE)
		return;


	switch (libinput_event_switch_get_switch_state(swev)) {
	case LIBINPUT_SWITCH_STATE_OFF:
		fallback_resume(dispatch, device);
		evdev_log_debug(device, "tablet-mode: resuming device\n");
		break;
	case LIBINPUT_SWITCH_STATE_ON:
		fallback_suspend(dispatch, device);
		evdev_log_debug(device, "tablet-mode: suspending device\n");
		break;
	}
}

static void
fallback_keyboard_pair_tablet_mode(struct evdev_device *keyboard,
				   struct evdev_device *tablet_mode_switch)
{
	struct fallback_dispatch *dispatch =
		fallback_dispatch(keyboard->dispatch);

	if ((keyboard->tags &
	     (EVDEV_TAG_TRACKPOINT|EVDEV_TAG_INTERNAL_KEYBOARD)) == 0)
		return;

	if ((tablet_mode_switch->tags & EVDEV_TAG_TABLET_MODE_SWITCH) == 0)
		return;

	if (dispatch->tablet_mode.other.sw_device)
		return;

	evdev_log_debug(keyboard,
			"tablet_mode_switch: activated for %s<->%s\n",
			keyboard->devname,
			tablet_mode_switch->devname);

	libinput_device_add_event_listener(&tablet_mode_switch->base,
				&dispatch->tablet_mode.other.listener,
				fallback_tablet_mode_switch_event,
				dispatch);
	dispatch->tablet_mode.other.sw_device = tablet_mode_switch;

	if (evdev_device_switch_get_state(tablet_mode_switch,
					  LIBINPUT_SWITCH_TABLET_MODE)
		    == LIBINPUT_SWITCH_STATE_ON)
		fallback_suspend(dispatch, keyboard);
}

static void
fallback_interface_device_added(struct evdev_device *device,
				struct evdev_device *added_device)
{
	fallback_lid_pair_keyboard(device, added_device);
	fallback_keyboard_pair_tablet_mode(device, added_device);
}

static void
fallback_interface_device_removed(struct evdev_device *device,
				  struct evdev_device *removed_device)
{
	struct fallback_dispatch *dispatch =
			fallback_dispatch(device->dispatch);
	struct paired_keyboard *kbd;

	ARRAY_FOR_EACH(dispatch->lid.paired_keyboard, kbd) {
		if (!kbd->device)
			continue;

		if (kbd->device != removed_device)
			continue;

		libinput_device_remove_event_listener(&kbd->listener);
		libinput_device_init_event_listener(&kbd->listener);
		kbd->device = NULL;
	}

	if (removed_device == dispatch->tablet_mode.other.sw_device) {
		libinput_device_remove_event_listener(
				&dispatch->tablet_mode.other.listener);
		libinput_device_init_event_listener(
				&dispatch->tablet_mode.other.listener);
		dispatch->tablet_mode.other.sw_device = NULL;
	}
}

struct evdev_dispatch_interface fallback_interface = {
	.process = fallback_interface_process,
	.suspend = fallback_interface_suspend,
	.remove = fallback_interface_remove,
	.destroy = fallback_interface_destroy,
	.device_added = fallback_interface_device_added,
	.device_removed = fallback_interface_device_removed,
	.device_suspended = fallback_interface_device_removed, /* treat as remove */
	.device_resumed = fallback_interface_device_added,   /* treat as add */
	.post_added = fallback_interface_sync_initial_state,
	.toggle_touch = fallback_interface_toggle_touch,
	.get_switch_state = fallback_interface_get_switch_state,
};

static void
fallback_change_to_left_handed(struct evdev_device *device)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(device->dispatch);

	if (device->left_handed.want_enabled == device->left_handed.enabled)
		return;

	if (fallback_any_button_down(dispatch, device))
		return;

	device->left_handed.enabled = device->left_handed.want_enabled;
}

static void
fallback_change_scroll_method(struct evdev_device *device)
{
	struct fallback_dispatch *dispatch = fallback_dispatch(device->dispatch);

	if (device->scroll.want_method == device->scroll.method &&
	    device->scroll.want_button == device->scroll.button)
		return;

	if (fallback_any_button_down(dispatch, device))
		return;

	device->scroll.method = device->scroll.want_method;
	device->scroll.button = device->scroll.want_button;
}

static int
fallback_rotation_config_is_available(struct libinput_device *device)
{
	/* This function only gets called when we support rotation */
	return 1;
}

static enum libinput_config_status
fallback_rotation_config_set_angle(struct libinput_device *libinput_device,
				unsigned int degrees_cw)
{
	struct evdev_device *device = evdev_device(libinput_device);
	struct fallback_dispatch *dispatch = fallback_dispatch(device->dispatch);

	dispatch->rotation.angle = degrees_cw;
	matrix_init_rotate(&dispatch->rotation.matrix, degrees_cw);

	return LIBINPUT_CONFIG_STATUS_SUCCESS;
}

static unsigned int
fallback_rotation_config_get_angle(struct libinput_device *libinput_device)
{
	struct evdev_device *device = evdev_device(libinput_device);
	struct fallback_dispatch *dispatch = fallback_dispatch(device->dispatch);

	return dispatch->rotation.angle;
}

static unsigned int
fallback_rotation_config_get_default_angle(struct libinput_device *device)
{
	return 0;
}

static void
fallback_init_rotation(struct fallback_dispatch *dispatch,
		       struct evdev_device *device)
{
	if ((device->model_flags & EVDEV_MODEL_TRACKBALL) == 0)
		return;

	dispatch->rotation.config.is_available = fallback_rotation_config_is_available;
	dispatch->rotation.config.set_angle = fallback_rotation_config_set_angle;
	dispatch->rotation.config.get_angle = fallback_rotation_config_get_angle;
	dispatch->rotation.config.get_default_angle = fallback_rotation_config_get_default_angle;
	dispatch->rotation.is_enabled = false;
	matrix_init_identity(&dispatch->rotation.matrix);
	device->base.config.rotation = &dispatch->rotation.config;
}

static inline int
fallback_dispatch_init_slots(struct fallback_dispatch *dispatch,
			     struct evdev_device *device)
{
	struct libevdev *evdev = device->evdev;
	struct mt_slot *slots;
	int num_slots;
	int active_slot;
	int slot;

	if (evdev_is_fake_mt_device(device) ||
	    !libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_X) ||
	    !libevdev_has_event_code(evdev, EV_ABS, ABS_MT_POSITION_Y))
		 return 0;

	/* We only handle the slotted Protocol B in libinput.
	   Devices with ABS_MT_POSITION_* but not ABS_MT_SLOT
	   require mtdev for conversion. */
	if (evdev_need_mtdev(device)) {
		device->mtdev = mtdev_new_open(device->fd);
		if (!device->mtdev)
			return -1;

		/* pick 10 slots as default for type A
		   devices. */
		num_slots = 10;
		active_slot = device->mtdev->caps.slot.value;
	} else {
		num_slots = libevdev_get_num_slots(device->evdev);
		active_slot = libevdev_get_current_slot(evdev);
	}

	slots = zalloc(num_slots * sizeof(struct mt_slot));

	for (slot = 0; slot < num_slots; ++slot) {
		slots[slot].seat_slot = -1;

		if (evdev_need_mtdev(device))
			continue;

		slots[slot].point.x = libevdev_get_slot_value(evdev,
							      slot,
							      ABS_MT_POSITION_X);
		slots[slot].point.y = libevdev_get_slot_value(evdev,
							      slot,
							      ABS_MT_POSITION_Y);
	}
	dispatch->mt.slots = slots;
	dispatch->mt.slots_len = num_slots;
	dispatch->mt.slot = active_slot;

	if (device->abs.absinfo_x->fuzz || device->abs.absinfo_y->fuzz) {
		dispatch->mt.want_hysteresis = true;
		dispatch->mt.hysteresis_margin.x = device->abs.absinfo_x->fuzz/2;
		dispatch->mt.hysteresis_margin.y = device->abs.absinfo_y->fuzz/2;
	}

	return 0;
}

static inline void
fallback_dispatch_init_rel(struct fallback_dispatch *dispatch,
			   struct evdev_device *device)
{
	dispatch->rel.x = 0;
	dispatch->rel.y = 0;
}

static inline void
fallback_dispatch_init_abs(struct fallback_dispatch *dispatch,
			   struct evdev_device *device)
{
	if (!libevdev_has_event_code(device->evdev, EV_ABS, ABS_X))
		return;

	dispatch->abs.point.x = device->abs.absinfo_x->value;
	dispatch->abs.point.y = device->abs.absinfo_y->value;
	dispatch->abs.seat_slot = -1;

	evdev_device_init_abs_range_warnings(device);
}

static inline void
fallback_dispatch_init_switch(struct fallback_dispatch *dispatch,
			      struct evdev_device *device)
{
	int val;

	if (device->tags & EVDEV_TAG_LID_SWITCH) {
		struct paired_keyboard *kbd;

		ARRAY_FOR_EACH(dispatch->lid.paired_keyboard, kbd)
			libinput_device_init_event_listener(&kbd->listener);

		dispatch->lid.reliability = evdev_read_switch_reliability_prop(device);
		dispatch->lid.is_closed = false;
	}

	if (device->tags & EVDEV_TAG_TABLET_MODE_SWITCH) {
		val = libevdev_get_event_value(device->evdev,
					       EV_SW,
					       SW_TABLET_MODE);
		dispatch->tablet_mode.sw.state = val;
	}

	libinput_device_init_event_listener(&dispatch->tablet_mode.other.listener);
}

struct evdev_dispatch *
fallback_dispatch_create(struct libinput_device *libinput_device)
{
	struct evdev_device *device = evdev_device(libinput_device);
	struct fallback_dispatch *dispatch;
	char timer_name[64];

	dispatch = zalloc(sizeof *dispatch);
	dispatch->device = evdev_device(libinput_device);
	dispatch->base.dispatch_type = DISPATCH_FALLBACK;
	dispatch->base.interface = &fallback_interface;
	dispatch->pending_event = EVDEV_NONE;

	fallback_dispatch_init_rel(dispatch, device);
	fallback_dispatch_init_abs(dispatch, device);
	if (fallback_dispatch_init_slots(dispatch, device) == -1) {
		free(dispatch);
		return NULL;
	}

	fallback_dispatch_init_switch(dispatch, device);

	if (device->left_handed.want_enabled)
		evdev_init_left_handed(device,
				       fallback_change_to_left_handed);

	if (device->scroll.want_button)
		evdev_init_button_scroll(device,
					 fallback_change_scroll_method);

	if (device->scroll.natural_scrolling_enabled)
		evdev_init_natural_scroll(device);

	evdev_init_calibration(device, &dispatch->calibration);
	evdev_init_sendevents(device, &dispatch->base);
	fallback_init_rotation(dispatch, device);

	/* BTN_MIDDLE is set on mice even when it's not present. So
	 * we can only use the absence of BTN_MIDDLE to mean something, i.e.
	 * we enable it by default on anything that only has L&R.
	 * If we have L&R and no middle, we don't expose it as config
	 * option */
	if (libevdev_has_event_code(device->evdev, EV_KEY, BTN_LEFT) &&
	    libevdev_has_event_code(device->evdev, EV_KEY, BTN_RIGHT)) {
		bool has_middle = libevdev_has_event_code(device->evdev,
							  EV_KEY,
							  BTN_MIDDLE);
		bool want_config = has_middle;
		bool enable_by_default = !has_middle;

		evdev_init_middlebutton(device,
					enable_by_default,
					want_config);
	}

	snprintf(timer_name,
		 sizeof(timer_name),
		 "%s debounce",
		 evdev_device_get_sysname(device));
	libinput_timer_init(&dispatch->debounce.timer,
			    evdev_libinput_context(device),
			    timer_name,
			    fallback_debounce_timeout,
			    device);
	return &dispatch->base;
}
