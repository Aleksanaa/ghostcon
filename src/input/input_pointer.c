#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <stdlib.h>
#include <time.h>
#include "input.h"
#include "input_internal.h"
#include "shl/eloop.h"
#include "shl/hook.h"

static void pointer_update_inactivity_timer(struct input_dev *dev)
{
	struct itimerspec spec;

	spec.it_interval.tv_nsec = 0;
	spec.it_interval.tv_sec = 0;
	spec.it_value.tv_nsec = 0;
	spec.it_value.tv_sec = 20;
	ev_timer_update(dev->input->hide_pointer, &spec);
}

static void pointer_dev_send_move(struct input_dev *dev)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_MOVED;
	pev.pointer_x = dev->pointer.x;
	pev.pointer_y = dev->pointer.y;

	/* Include button state if a button is pressed during motion (drag) */
	if (dev->pointer.pressed_button != BUTTON_NONE) {
		pev.button = dev->pointer.pressed_button;
		pev.pressed = true;
	} else {
		pev.button = 0;
		pev.pressed = false;
	}

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
}

static void pointer_dev_send_wheel(struct input_dev *dev, int32_t value)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_WHEEL;
	pev.wheel = value;

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
}

static void pointer_dev_send_button(struct input_dev *dev, uint8_t button, bool pressed,
				    bool dbl_click)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_BUTTON;
	pev.button = button;
	pev.pressed = pressed;
	pev.double_click = dbl_click;

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
}

void pointer_dev_sync(struct input_dev *dev)
{
	struct input_pointer_event pev = {0};

	pev.event = POINTER_SYNC;

	shl_hook_call(dev->input->pointer_hook, dev->input, &pev);
	pointer_update_inactivity_timer(dev);
}

void pointer_dev_rel(struct input_dev *dev, uint16_t code, int32_t value)
{
	switch (code) {
	case REL_X:
		dev->pointer.x += value;
		if (dev->pointer.x < 0)
			dev->pointer.x = 0;
		if (dev->pointer.x > dev->input->pointer_max_x)
			dev->pointer.x = dev->input->pointer_max_x;
		pointer_dev_send_move(dev);
		break;
	case REL_Y:
		dev->pointer.y += value;
		if (dev->pointer.y < 0)
			dev->pointer.y = 0;
		if (dev->pointer.y > dev->input->pointer_max_y)
			dev->pointer.y = dev->input->pointer_max_y;
		pointer_dev_send_move(dev);
		break;
	case REL_WHEEL:
		pointer_dev_send_wheel(dev, value);
		break;
	default:
		break;
	}
}

/*
 * A tap has to be short and nearly still. The travel limit is taken from the
 * size of the pad rather than a pixel count, so it means the same thing on a
 * small pad as on a large one: roughly a fortieth of the way across.
 */
#define TAP_TIMEOUT_MS 180

static int32_t tap_travel_limit(struct input_dev *dev)
{
	int32_t span = dev->pointer.max_x - dev->pointer.min_x;

	return span > 0 ? span / 40 : 0;
}

/*
 * How far two fingers travel for one wheel click. A twentieth of the pad gives
 * a full swipe about twenty steps, which lands close to what a mouse wheel
 * does over the same gesture.
 */
static int32_t scroll_step(struct input_dev *dev)
{
	int32_t span = dev->pointer.max_y - dev->pointer.min_y;

	return span > 0 ? span / 20 : 0;
}

/*
 * A clickpad has a single button under the whole surface, so every press
 * arrives as BTN_LEFT and the number of fingers resting on the pad is what
 * separates the three buttons. Taps are read the same way. This is the
 * convention every other touchpad stack uses, so it is the one users expect.
 */
static uint8_t fingers_to_button(uint8_t fingers)
{
	switch (fingers) {
	case 2:
		return 2; /* two fingers: right */
	case 3:
		return 1; /* three fingers: middle */
	default:
		return 0; /* one finger, or a device with real buttons: left */
	}
}

static uint8_t click_button(struct input_dev *dev)
{
	return fingers_to_button(dev->pointer.fingers);
}

/* Was the contact that just ended a tap rather than a drag or a press? */
static bool touch_was_tap(struct input_dev *dev)
{
	struct timespec tp;
	uint64_t elapsed;

	if (dev->pointer.touch_clicked || !dev->pointer.tap_fingers)
		return false;
	if (dev->pointer.touch_travel > tap_travel_limit(dev))
		return false;

	clock_gettime(CLOCK_MONOTONIC, &tp);
	elapsed = (tp.tv_sec - dev->pointer.touch_start.tv_sec) * 1000 +
		  (tp.tv_nsec - dev->pointer.touch_start.tv_nsec) / 1000000;
	return elapsed < TAP_TIMEOUT_MS;
}

/* A tap is a press and a release in one go, with nothing in between. */
static void pointer_dev_tap(struct input_dev *dev)
{
	struct timespec tp;
	uint64_t elapsed;
	uint8_t button = fingers_to_button(dev->pointer.tap_fingers);
	bool dbl_click = false;

	clock_gettime(CLOCK_MONOTONIC, &tp);
	elapsed = (tp.tv_sec - dev->pointer.last_click.tv_sec) * 1000 +
		  (tp.tv_nsec - dev->pointer.last_click.tv_nsec) / 1000000;
	dev->pointer.last_click = tp;

	/* only a plain tap can grow into a word selection */
	if (!button)
		dbl_click = (elapsed < 500);

	pointer_dev_send_button(dev, button, true, dbl_click);
	pointer_dev_send_button(dev, button, false, false);
	dev->pointer.pressed_button = BUTTON_NONE;
}

/*
 * Two fingers on the pad scroll instead of moving the pointer. The motion is
 * banked until it is worth a whole wheel click, so a slow drag still scrolls
 * smoothly rather than not at all.
 */
static void pointer_dev_scroll(struct input_dev *dev, int32_t dy)
{
	int32_t step = scroll_step(dev);

	if (step <= 0)
		return;

	dev->pointer.scroll_accum += dy;
	while (dev->pointer.scroll_accum >= step) {
		dev->pointer.scroll_accum -= step;
		pointer_dev_send_wheel(dev, -1);
	}
	while (dev->pointer.scroll_accum <= -step) {
		dev->pointer.scroll_accum += step;
		pointer_dev_send_wheel(dev, 1);
	}
}

static void pointer_dev_abs_x(struct input_dev *dev, int32_t value)
{
	switch (dev->pointer.kind) {
	case POINTER_TOUCHPAD:
		if (dev->pointer.touchpad_needs_sync_off_x) {
			dev->pointer.off_x = dev->pointer.x - value;
			dev->pointer.touch_prev_x = value;
			dev->pointer.touchpad_needs_sync_off_x = false;
		}

		dev->pointer.touch_travel += abs(value - dev->pointer.touch_prev_x);
		dev->pointer.touch_prev_x = value;

		/* two fingers scroll, and scrolling must not drag the pointer
		 * along with it */
		if (dev->pointer.fingers > 1)
			return;

		if (dev->pointer.touchpaddown) {
			dev->pointer.x = dev->pointer.off_x + value;
			if (dev->pointer.x < 0) {
				dev->pointer.x = 0;
				dev->pointer.off_x = -value;
			}
			if (dev->pointer.x > dev->input->pointer_max_x) {
				dev->pointer.x = dev->input->pointer_max_x;
				dev->pointer.off_x = dev->input->pointer_max_x - value;
			}
		}
		break;
	case POINTER_TOUCHSCREEN:
	case POINTER_VMOUSE:
		dev->pointer.x = ((value - dev->pointer.min_x) * dev->input->pointer_max_x) /
				 (dev->pointer.max_x - dev->pointer.min_x);
		break;
	default:
		return;
	}
	pointer_dev_send_move(dev);
}

static void pointer_dev_abs_y(struct input_dev *dev, int32_t value)
{
	int32_t dy;

	switch (dev->pointer.kind) {
	case POINTER_TOUCHPAD:
		if (dev->pointer.touchpad_needs_sync_off_y) {
			dev->pointer.off_y = dev->pointer.y - value;
			dev->pointer.touch_prev_y = value;
			dev->pointer.touchpad_needs_sync_off_y = false;
		}

		dy = value - dev->pointer.touch_prev_y;
		dev->pointer.touch_travel += abs(dy);
		dev->pointer.touch_prev_y = value;

		if (dev->pointer.fingers > 1) {
			pointer_dev_scroll(dev, dy);
			return;
		}

		if (dev->pointer.touchpaddown) {
			dev->pointer.y = dev->pointer.off_y + value;
			if (dev->pointer.y < 0) {
				dev->pointer.y = 0;
				dev->pointer.off_y = -value;
			}
			if (dev->pointer.y > dev->input->pointer_max_y) {
				dev->pointer.y = dev->input->pointer_max_y;
				dev->pointer.off_y = dev->input->pointer_max_y - value;
			}
		}
		break;
	case POINTER_TOUCHSCREEN:
	case POINTER_VMOUSE:
		dev->pointer.y = ((value - dev->pointer.min_y) * dev->input->pointer_max_y) /
				 (dev->pointer.max_y - dev->pointer.min_y);
		break;
	default:
		return;
	}
	pointer_dev_send_move(dev);
}

void pointer_dev_abs(struct input_dev *dev, uint16_t code, int32_t value)
{
	switch (code) {
	case ABS_X:
		pointer_dev_abs_x(dev, value);
		break;
	case ABS_Y:
		pointer_dev_abs_y(dev, value);
		break;
	}
}

/*
 * Fingers landing or leaving changes which one the kernel reports as the
 * primary contact, and its position can be anywhere on the pad. Re-anchor the
 * offset so the pointer carries on from where it is instead of jumping.
 */
static void pointer_dev_set_fingers(struct input_dev *dev, uint8_t fingers)
{
	if (dev->pointer.fingers == fingers)
		return;

	dev->pointer.fingers = fingers;
	dev->pointer.touchpad_needs_sync_off_x = true;
	dev->pointer.touchpad_needs_sync_off_y = true;
	dev->pointer.scroll_accum = 0;

	if (fingers > dev->pointer.tap_fingers)
		dev->pointer.tap_fingers = fingers;
}

void pointer_dev_button(struct input_dev *dev, uint16_t code, int32_t value)
{
	struct timespec tp;
	uint64_t elapsed;
	bool pressed = (value == 1);
	bool dbl_click = false;
	uint8_t button;

	/* Only handle press and release events */
	if (value > 1)
		return;

	switch (code) {
	/*
	 * Finger counts arrive as tool state, not as buttons: they say how many
	 * fingers rest on the pad, which is true long before and after any
	 * click. Record them and let the click or the tap decide what to send.
	 */
	case BTN_TOOL_FINGER:
		if (pressed)
			pointer_dev_set_fingers(dev, 1);
		break;
	case BTN_TOOL_DOUBLETAP:
		if (pressed)
			pointer_dev_set_fingers(dev, 2);
		break;
	case BTN_TOOL_TRIPLETAP:
		if (pressed)
			pointer_dev_set_fingers(dev, 3);
		break;
	case BTN_LEFT:
		if (pressed) {
			clock_gettime(CLOCK_MONOTONIC, &tp);
			elapsed = (tp.tv_sec - dev->pointer.last_click.tv_sec) * 1000 +
				  (tp.tv_nsec - dev->pointer.last_click.tv_nsec) / 1000000;
			dbl_click = (elapsed < 500);
			dev->pointer.last_click = tp;
			dev->pointer.pressed_button = click_button(dev);
			dev->pointer.touch_clicked = true;
			button = dev->pointer.pressed_button;
			/* only a plain click starts a selection */
			if (button != 0)
				dbl_click = false;
		} else {
			/*
			 * The release has to name the button the press sent.
			 * Fingers usually leave the pad before it is let go, so
			 * asking the finger count again here would report the
			 * wrong button.
			 */
			button = dev->pointer.pressed_button;
			if (button == BUTTON_NONE)
				button = 0;
			dev->pointer.pressed_button = BUTTON_NONE;
		}
		pointer_dev_send_button(dev, button, pressed, dbl_click);
		break;
	case BTN_RIGHT:
		dev->pointer.pressed_button = pressed ? 2 : BUTTON_NONE; /* Button 2 = right */
		pointer_dev_send_button(dev, 2, pressed, false);
		break;
	case BTN_MIDDLE:
		dev->pointer.pressed_button = pressed ? 1 : BUTTON_NONE; /* Button 1 = middle */
		pointer_dev_send_button(dev, 1, pressed, false);
		break;
	case BTN_TOUCH:
		dev->pointer.touchpaddown = pressed;
		if (pressed) {
			dev->pointer.touchpad_needs_sync_off_x = true;
			dev->pointer.touchpad_needs_sync_off_y = true;
			clock_gettime(CLOCK_MONOTONIC, &dev->pointer.touch_start);
			dev->pointer.touch_travel = 0;
			dev->pointer.touch_clicked = false;
			dev->pointer.scroll_accum = 0;
			/*
			 * tap_fingers is deliberately not cleared here. Event
			 * codes are reported in ascending order within a frame,
			 * so BTN_TOOL_FINGER has already counted this contact by
			 * the time BTN_TOUCH arrives; clearing it now would lose
			 * the count and no tap would ever be seen.
			 */
		} else {
			/*
			 * Everything has left the pad. If the contact was brief
			 * and still, the user meant it as a click even though
			 * they never pushed the button down.
			 */
			if (touch_was_tap(dev))
				pointer_dev_tap(dev);
			dev->pointer.fingers = 0;
			dev->pointer.tap_fingers = 0;
		}
		break;
	default:
		break;
	}
}
