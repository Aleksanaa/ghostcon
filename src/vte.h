/*
 * kmscon - VT layer
 *
 * Copyright (c) 2011-2012 David Herrmann <dh.herrmann@googlemail.com>
 * Copyright (c) 2011 University of Tuebingen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * VT layer
 * This wraps libghostty-vt and provides the terminal state machine, the
 * keyboard and mouse encoders, selection handling and a flattened cell
 * array that the text renderers draw.
 */

#ifndef KMSCON_VTE_H
#define KMSCON_VTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Palette entries that can be configured by the user. The first 16 entries
 * match the ANSI color codes, the last two are the default fore- and
 * background colors. */
enum kmscon_color_code {
	KMSCON_COLOR_BLACK,
	KMSCON_COLOR_RED,
	KMSCON_COLOR_GREEN,
	KMSCON_COLOR_YELLOW,
	KMSCON_COLOR_BLUE,
	KMSCON_COLOR_MAGENTA,
	KMSCON_COLOR_CYAN,
	KMSCON_COLOR_LIGHT_GREY,
	KMSCON_COLOR_DARK_GREY,
	KMSCON_COLOR_LIGHT_RED,
	KMSCON_COLOR_LIGHT_GREEN,
	KMSCON_COLOR_LIGHT_YELLOW,
	KMSCON_COLOR_LIGHT_BLUE,
	KMSCON_COLOR_LIGHT_MAGENTA,
	KMSCON_COLOR_LIGHT_CYAN,
	KMSCON_COLOR_WHITE,

	KMSCON_COLOR_FOREGROUND,
	KMSCON_COLOR_BACKGROUND,

	KMSCON_COLOR_NUM
};

struct kmscon_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
};

/* Attributes that alter the glyph shape */
typedef union {
	struct {
		uint8_t bold : 1;
		uint8_t italic : 1;
		uint8_t underline : 1;
		uint8_t blink : 1;
		/* not a character attribute, this asks the renderers for a bar
		 * along the left edge of the cell, that is a bar cursor */
		uint8_t cursor_bar : 1;
		uint8_t reserved : 3;
	};
	uint8_t u8;
} kmscon_attr_t;

struct kmscon_cell {
	uint32_t ch;		/* character, 0 if the cell is empty */
	struct kmscon_color fg; /* foreground color */
	struct kmscon_color bg; /* background color */
	kmscon_attr_t attr;	/* glyph attributes */
};

/* Default colors of a screen, used to clear unused areas */
struct kmscon_screen_attr {
	struct kmscon_color fg;
	struct kmscon_color bg;
};

enum kmscon_cursor_shape {
	KMSCON_CURSOR_BLOCK,
	KMSCON_CURSOR_UNDERLINE,
	KMSCON_CURSOR_BAR,
	KMSCON_CURSOR_BLOCK_HOLLOW,
};

/* Position of the viewport inside the scrollback */
struct kmscon_vte_scrollbar {
	uint64_t total;	 /* size of the scrollable area in rows */
	uint64_t offset; /* first visible row inside that area */
	uint64_t len;	 /* number of visible rows */
};

/* Snapshot of the visible screen, valid until the next kmscon_vte_draw() */
struct kmscon_vte_screen {
	const struct kmscon_cell *cells;
	unsigned int cols;
	unsigned int rows;

	unsigned int cursor_x;
	unsigned int cursor_y;
	bool cursor_visible;
	bool cursor_blinks;
	enum kmscon_cursor_shape cursor_shape;
	/* the cursor is drawn by inverting the cell it sits on unless a color
	 * was configured or requested by the application */
	bool cursor_has_color;
	struct kmscon_color cursor_color;
};

enum kmscon_mouse_event {
	KMSCON_MOUSE_PRESSED,
	KMSCON_MOUSE_RELEASED,
	KMSCON_MOUSE_MOVED,
};

struct kmscon_vte;

typedef void (*kmscon_vte_write_cb)(const char *u8, size_t len, void *data);
typedef void (*kmscon_vte_bell_cb)(void *data);
typedef void (*kmscon_vte_osc_cb)(const char *osc, size_t len, void *data);
typedef void (*kmscon_vte_mouse_mode_cb)(bool tracking, void *data);

/* The application asked us to put @u8 into the copy buffer, a @len of zero
 * asks for the buffer to be cleared. */
typedef void (*kmscon_vte_copy_cb)(const char *u8, size_t len, void *data);

int kmscon_vte_new(struct kmscon_vte **out, unsigned int cols, unsigned int rows,
		   unsigned int max_scrollback, void *data);
void kmscon_vte_free(struct kmscon_vte *vte);

void kmscon_vte_set_write_cb(struct kmscon_vte *vte, kmscon_vte_write_cb cb);
void kmscon_vte_set_bell_cb(struct kmscon_vte *vte, kmscon_vte_bell_cb cb);
void kmscon_vte_set_osc_cb(struct kmscon_vte *vte, kmscon_vte_osc_cb cb);
void kmscon_vte_set_mouse_mode_cb(struct kmscon_vte *vte, kmscon_vte_mouse_mode_cb cb);

/* Registering a copy callback enables OSC 52, leaving it unset makes the
 * terminal ignore clipboard writes from applications. */
void kmscon_vte_set_copy_cb(struct kmscon_vte *vte, kmscon_vte_copy_cb cb);

int kmscon_vte_set_palette(struct kmscon_vte *vte, const char *name, const uint8_t (*custom)[3]);
void kmscon_vte_set_backspace_sends_delete(struct kmscon_vte *vte, bool set);
void kmscon_vte_set_min_contrast(struct kmscon_vte *vte, double ratio);

/* Defaults for the cursor, applications can still change all of them at
 * runtime via DECSCUSR and OSC 12. @name is one of "block", "underline",
 * "bar" or "hollow", a NULL @rgb clears the cursor color again. */
int kmscon_vte_set_cursor_shape(struct kmscon_vte *vte, const char *name);
void kmscon_vte_set_cursor_blink(struct kmscon_vte *vte, bool blink);
void kmscon_vte_set_cursor_color(struct kmscon_vte *vte, const uint8_t rgb[3]);

void kmscon_vte_set_scrollbar(struct kmscon_vte *vte, bool set);

/* Parse a color in any of the formats that libghostty-vt understands, that is
 * X11 color names, hex colors with an optional leading '#', rgb:<r>/<g>/<b>
 * and rgbi:<r>/<g>/<b>. */
int kmscon_vte_parse_color(const char *value, uint8_t out[3]);

void kmscon_vte_input(struct kmscon_vte *vte, const char *u8, size_t len);

/* Report focus changes to the application if it enabled focus events */
void kmscon_vte_set_focus(struct kmscon_vte *vte, bool focused);

/* Release the memory of scrollback that is not currently displayed */
void kmscon_vte_compress_scrollback(struct kmscon_vte *vte);
void kmscon_vte_hard_reset(struct kmscon_vte *vte);
void kmscon_vte_paste(struct kmscon_vte *vte, const char *u8, size_t len);

int kmscon_vte_resize(struct kmscon_vte *vte, unsigned int cols, unsigned int rows,
		      unsigned int cell_width, unsigned int cell_height);
unsigned int kmscon_vte_get_cols(struct kmscon_vte *vte);
unsigned int kmscon_vte_get_rows(struct kmscon_vte *vte);
void kmscon_vte_get_def_attr(struct kmscon_vte *vte, struct kmscon_screen_attr *out);

int kmscon_vte_draw(struct kmscon_vte *vte, struct kmscon_vte_screen *out);
void kmscon_vte_get_scrollbar(struct kmscon_vte *vte, struct kmscon_vte_scrollbar *out);

void kmscon_vte_sb_up(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_down(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_page_up(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_page_down(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_reset(struct kmscon_vte *vte);

void kmscon_vte_selection_reset(struct kmscon_vte *vte);
void kmscon_vte_selection_start(struct kmscon_vte *vte, unsigned int x, unsigned int y);
void kmscon_vte_selection_target(struct kmscon_vte *vte, unsigned int x, unsigned int y);
void kmscon_vte_selection_word(struct kmscon_vte *vte, unsigned int x, unsigned int y);
int kmscon_vte_selection_copy(struct kmscon_vte *vte, char **out);

bool kmscon_vte_handle_keyboard(struct kmscon_vte *vte, uint16_t keycode, uint32_t ascii,
				uint32_t unicode, unsigned int mods);
bool kmscon_vte_get_mouse_tracking(struct kmscon_vte *vte);
void kmscon_vte_handle_mouse(struct kmscon_vte *vte, int32_t pixel_x, int32_t pixel_y,
			     unsigned int button, enum kmscon_mouse_event event, unsigned int mods);

#endif /* KMSCON_VTE_H */
