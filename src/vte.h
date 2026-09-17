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
 * keyboard and mouse encoders, selection handling and the frames that the
 * text renderers draw.
 *
 * A frame is pulled row by row, and every row says whether it changed since
 * the renderer last saw it, so that a renderer which caches what it drew only
 * pays for the part of the screen that actually moved.
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

/* Position of the viewport inside the scrollback */
struct kmscon_vte_scrollbar {
	uint64_t total;	 /* size of the scrollable area in rows */
	uint64_t offset; /* first visible row inside that area */
	uint64_t len;	 /* number of visible rows */
};

/* A renderer's place in the terminal's history of changes. Several displays
 * can show the same terminal, so every renderer keeps one of these and learns
 * from it which rows it still owes a redraw. A zeroed watch has seen nothing
 * and is handed a full frame. */
struct kmscon_vte_watch {
	uint64_t seq;
};

/* The visible screen, valid from kmscon_vte_frame_begin() until the next one */
struct kmscon_vte_frame {
	unsigned int cols;
	unsigned int rows;
	/* default colors, unused parts of the screen are cleared with these */
	struct kmscon_screen_attr attr;
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

/* The two blink timers. Every tick flips a phase and marks exactly what that
 * phase affects as changed, so the next frame repaints only the blinking cells
 * or the row the cursor is on. The cursor goes solid again while the user is
 * typing, which is what the reset is for. */
void kmscon_vte_blink_tick(struct kmscon_vte *vte);
void kmscon_vte_cursor_blink_tick(struct kmscon_vte *vte);
void kmscon_vte_cursor_blink_reset(struct kmscon_vte *vte);

/* Pull the terminal into the render state and describe the frame to draw.
 * Returns 1 when something changed since @watch last drew, 0 when the screen
 * is unchanged, or a negative error code. A renderer with a reason of its own
 * to redraw may iterate the frame even when this returns 0. */
int kmscon_vte_frame_begin(struct kmscon_vte *vte, const struct kmscon_vte_watch *watch,
			   struct kmscon_vte_frame *out);

/* Walk the rows of the current frame from top to bottom. @dirty tells whether
 * the row changed since @watch last drew it. */
bool kmscon_vte_frame_row(struct kmscon_vte *vte, unsigned int *y, bool *dirty);

/* Flatten the current row into @cells, which holds @len entries. At most
 * frame.cols of them are written. Selection, contrast, blinking, the scrollbar
 * and the cursor are already folded in, so a renderer only has to turn cells
 * into glyphs. */
void kmscon_vte_frame_cells(struct kmscon_vte *vte, struct kmscon_cell *cells, unsigned int len);

/* Remember that @watch has drawn the frame that was just iterated */
void kmscon_vte_frame_end(struct kmscon_vte *vte, struct kmscon_vte_watch *watch);

void kmscon_vte_get_scrollbar(struct kmscon_vte *vte, struct kmscon_vte_scrollbar *out);

void kmscon_vte_sb_up(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_down(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_page_up(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_page_down(struct kmscon_vte *vte, unsigned int num);
void kmscon_vte_sb_reset(struct kmscon_vte *vte);

/* Scroll to the @delta'th shell prompt above (negative) or below (positive)
 * the top of the viewport. Needs a shell that emits OSC 133. */
int kmscon_vte_jump_to_prompt(struct kmscon_vte *vte, int delta);

void kmscon_vte_selection_reset(struct kmscon_vte *vte);
void kmscon_vte_selection_start(struct kmscon_vte *vte, unsigned int x, unsigned int y);
void kmscon_vte_selection_target(struct kmscon_vte *vte, unsigned int x, unsigned int y);
void kmscon_vte_selection_word(struct kmscon_vte *vte, unsigned int x, unsigned int y);

/* Select the whole output of the command below the given position. Needs a
 * shell that emits OSC 133. */
void kmscon_vte_selection_output(struct kmscon_vte *vte, unsigned int x, unsigned int y);
int kmscon_vte_selection_copy(struct kmscon_vte *vte, char **out);

bool kmscon_vte_handle_keyboard(struct kmscon_vte *vte, uint16_t keycode, uint32_t ascii,
				uint32_t unicode, unsigned int mods);
bool kmscon_vte_get_mouse_tracking(struct kmscon_vte *vte);
void kmscon_vte_handle_mouse(struct kmscon_vte *vte, int32_t pixel_x, int32_t pixel_y,
			     unsigned int button, enum kmscon_mouse_event event, unsigned int mods);

#endif /* KMSCON_VTE_H */
