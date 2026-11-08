/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>

struct keyboard;
/* Wire format: four bytes 'Q', 1 (version), Shift/Caps flags, Sym flags.
 * Layer flags: 1 held, 2 oneshot, 4 toggled. Shift also has 8 for CapsLock LED.
 * No keycodes, input text, commands, or incremental events cross this socket. */
int q25_state_init(const char *path);
void q25_state_accept(void);
void q25_state_publish(uint8_t shift, uint8_t sym);
int q25_state_layers(const struct keyboard *kbd, uint8_t *shift, uint8_t *sym);
