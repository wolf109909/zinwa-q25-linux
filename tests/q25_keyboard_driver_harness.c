/* SPDX-License-Identifier: GPL-2.0-only
 * Compile the actual IRQ worker/keymap with synthetic FIFO and trackpad motion.
 * Hardware/kernel IO is replaced; no physical event source is opened. */
#include <linux/input-event-codes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/keyboard/config.h"
#include "../drivers/keyboard/bbq20kbd_pmod_codes.h"
#include "../drivers/keyboard/bbqX0kbd_registers.h"
#define BBQX0KBD_I2C_ADDRESS BBQX0KBD_ASSIGNED_I2C_ADDRESS
#define BIT(n) (1u << (n))
#define LEFT_CTRL_BIT BIT(0)
#define LEFT_SHIFT_BIT BIT(1)
#define LEFT_ALT_BIT BIT(2)
#define RIGHT_CTRL_BIT BIT(4)
#define RIGHT_SHIFT_BIT BIT(5)
#define RIGHT_ALT_BIT BIT(6)
#define fallthrough __attribute__((fallthrough))
#define container_of(ptr, type, field) ((type *)((char *)(ptr) - offsetof(type, field)))
#define BBQX0KBD_KEYBOARD_LOG(...) ((void)0)
#define BBQX0KBD_KEYBOARD_ERR(...) abort()
struct work_struct {int unused;};
struct input_dev {int unused;};
struct i2c_client {int unused;};
struct bbqX0kbd_data {
	struct work_struct work_struct;
	struct input_dev *input_dev;
	struct i2c_client *i2c_client;
	uint8_t fifoCount, fifoData[16][2], modifier_keys_status, touchInt;
	int8_t rel_x, rel_y, relX_offset, relY_offset;
	unsigned short keycode[256];
	int q20_spec_switch_key_mouse;
};
static struct bbqX0kbd_data driver;
static int timestamp, mouse_code, relative_events;
extern void test_event(int code, int pressed, int time);
static void input_report_key(struct input_dev *dev, unsigned short code, int down)
{
	if (code < 256)
		test_event(code, down, timestamp);
	else
		mouse_code = code;
}
static void input_report_rel(struct input_dev *dev, int axis, int value) { relative_events++; }
static void input_event(struct input_dev *dev, int type, int code, int value) {}
static void input_sync(struct input_dev *dev) {}
static int bbqX0kbd_write(struct i2c_client *client, int address, int reg, void *value, int size) { return 0; }
/* Generated from source, not a copied/reimplemented key transformation. */
#include "driver-test-functions.c"

void driver_init(void)
{
	memset(&driver, 0, sizeof(driver));
	memcpy(driver.keycode, keycodes, sizeof(keycodes));
	driver.q20_spec_switch_key_mouse = 1;
	mouse_code = relative_events = 0;
}
void driver_key(int scan, int down, int time)
{
	timestamp = time;
	driver.fifoData[0][0] = down ? KEY_PRESSED_STATE : KEY_RELEASED_STATE;
	driver.fifoData[0][1] = scan;
	driver.fifoCount = 1;
	bbqX0kbd_work_fnc(&driver.work_struct);
}
void driver_motion(int x, int y, int time)
{
	timestamp = time;
	driver.touchInt = 1;
	driver.rel_x = x;
	driver.rel_y = y;
	bbqX0kbd_work_fnc(&driver.work_struct);
}
int driver_mouse_code(void) { return mouse_code; }
int driver_relative_events(void) { return relative_events; }
