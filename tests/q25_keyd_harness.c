/* SPDX-License-Identifier: MIT
 * Run the real daemon event handler and keyd engine, replacing ONLY hardware IO.
 * Never opens /dev/input or /dev/uinput and never contacts a running daemon. */
#include "daemon.c"

struct device device_table[MAX_DEVICES];
size_t device_table_sz;
static struct {uint8_t code, state;} captured[4096];
static int ncaptured;

void vkbd_send_key(const struct vkbd *v, uint8_t code, int state)
{
	assert(ncaptured < ARRAY_SIZE(captured));
	captured[ncaptured].code = code;
	captured[ncaptured++].state = state;
}
void vkbd_mouse_move(const struct vkbd *v, int x, int y) {}
void vkbd_mouse_move_abs(const struct vkbd *v, int x, int y) {}
void vkbd_mouse_scroll(const struct vkbd *v, int x, int y) {}
struct vkbd *vkbd_init(const char *name) { abort(); }
void free_vkbd(struct vkbd *v) {}
int device_grab(struct device *d) { return 0; }
int device_ungrab(struct device *d) { return 0; }
void device_set_led(const struct device *d, int led, int state) {}
void evloop_add_fd(int fd) { abort(); }
int evloop(int (*handler)(struct event *)) { abort(); }

void test_setup(void)
{
	device_table_sz = 1;
	device_table[0] = (struct device){.capabilities = CAP_KEY | CAP_KEYBOARD};
	strcpy(device_table[0].id, "0001:0001:45028087");
	q25_capslock = 0;
	reload();
	assert(device_table[0].data);
	ncaptured = 0;
}

int test_code(const char *name)
{
	for (int i = 1; i < 256; i++)
		if ((keycode_table[i].name && !strcmp(keycode_table[i].name, name)) ||
		    (keycode_table[i].alt_name && !strcmp(keycode_table[i].alt_name, name)))
			return i;
	return -1;
}

void test_event(int code, int pressed, int time)
{
	struct device_event key = {.type = DEV_KEY, .code = code, .pressed = pressed};
	struct event ev = {.type = code ? EV_DEV_EVENT : EV_TIMEOUT, .timestamp = time,
		.dev = &device_table[0], .devev = &key};
	event_handler(&ev);
}

void test_remove(int time)
{
	struct event ev = {.type = EV_DEV_REMOVE, .timestamp = time, .dev = &device_table[0]};
	event_handler(&ev);
}

void test_reload(void) { reload(); }
void test_clear(void)
{
	/* Via the real parser/engine, not synthetic layer-change notifications. */
	struct keyboard *kbd = device_table[0].data;
	assert(kbd_eval(kbd, "main.f24 = clear()") == 0);
}

void test_led(int value, int time)
{
	struct device virtual = {.is_virtual = 1};
	struct device_event led = {.type = DEV_LED, .code = LED_CAPSL, .pressed = value};
	struct event ev = {.type = EV_DEV_EVENT, .timestamp = time, .dev = &virtual, .devev = &led};
	event_handler(&ev);
}

int test_output_count(void) { return ncaptured; }
int test_output_code(int i) { return captured[i].code; }
int test_output_state(int i) { return captured[i].state; }
const char *test_name(int code)
{
	return code == KEYD_BACKSLASH ? "backslash" : keycode_table[code].name;
}
void test_reset_output(void) { ncaptured = 0; }
int test_layer_flags(void)
{
	uint8_t shift = 0, sym = 0;
	if (device_table[0].data)
		q25_state_layers(device_table[0].data, &shift, &sym);
	return shift | (sym << 8);
}
