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

#include <errno.h>
#include <ghostty/vt.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include "input/input.h"
#include "shl/log.h"
#include "shl/misc.h"
#include "vte.h"

#define LOG_SUBSYSTEM "vte"

#define OSC_MAX 256
#define ENCODE_MAX 256

/* We can only paste plain text, so this is the representation we look for in
 * a clipboard write. */
#define CLIPBOARD_TEXT "text/plain"
#define CLIPBOARD_TEXT_LEN (sizeof(CLIPBOARD_TEXT) - 1)

enum osc_state {
	OSC_NONE,
	OSC_ESC,
	OSC_BODY,
	OSC_TERM,
};

struct kmscon_vte {
	void *data;

	kmscon_vte_write_cb write_cb;
	kmscon_vte_bell_cb bell_cb;
	kmscon_vte_osc_cb osc_cb;
	kmscon_vte_mouse_mode_cb mouse_mode_cb;
	kmscon_vte_copy_cb copy_cb;

	GhosttyTerminal term;
	GhosttyRenderState render;
	GhosttyRenderStateRowIterator rows;
	GhosttyRenderStateRowCells row_cells;
	GhosttyKeyEncoder key_enc;
	GhosttyKeyEvent key_ev;
	GhosttyMouseEncoder mouse_enc;
	GhosttyMouseEvent mouse_ev;
	GhosttyTrackedGridRef sel_anchor;

	struct kmscon_cell *cells;
	size_t cells_count;
	uint32_t *graphemes;
	size_t graphemes_count;

	unsigned int cols;
	unsigned int rows_num;
	unsigned int cell_width;
	unsigned int cell_height;

	bool backspace_sends_delete;
	bool mouse_tracking;
	bool any_button_pressed;
	bool focused;
	bool scrollbar;
	bool vt_error_logged;
	double min_contrast;

	enum osc_state osc_state;
	char osc_buf[OSC_MAX];
	size_t osc_len;
};

/*
 * Color palettes
 * These are the palettes that kmscon has always shipped. Only the first 16
 * entries plus the default fore- and background are configurable, the rest of
 * the 256 color palette is taken from libghostty-vt.
 */

static uint8_t color_palette_legacy[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {0, 0, 0},
	[KMSCON_COLOR_RED] = {205, 0, 0},
	[KMSCON_COLOR_GREEN] = {0, 205, 0},
	[KMSCON_COLOR_YELLOW] = {205, 205, 0},
	[KMSCON_COLOR_BLUE] = {0, 0, 238},
	[KMSCON_COLOR_MAGENTA] = {205, 0, 205},
	[KMSCON_COLOR_CYAN] = {0, 205, 205},
	[KMSCON_COLOR_LIGHT_GREY] = {229, 229, 229},
	[KMSCON_COLOR_DARK_GREY] = {127, 127, 127},
	[KMSCON_COLOR_LIGHT_RED] = {255, 0, 0},
	[KMSCON_COLOR_LIGHT_GREEN] = {0, 255, 0},
	[KMSCON_COLOR_LIGHT_YELLOW] = {255, 255, 0},
	[KMSCON_COLOR_LIGHT_BLUE] = {92, 92, 255},
	[KMSCON_COLOR_LIGHT_MAGENTA] = {255, 0, 255},
	[KMSCON_COLOR_LIGHT_CYAN] = {0, 255, 255},
	[KMSCON_COLOR_WHITE] = {255, 255, 255},

	[KMSCON_COLOR_FOREGROUND] = {229, 229, 229},
	[KMSCON_COLOR_BACKGROUND] = {0, 0, 0},
};

static uint8_t color_palette_vga[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {0, 0, 0},
	[KMSCON_COLOR_RED] = {170, 0, 0},
	[KMSCON_COLOR_GREEN] = {0, 170, 0},
	[KMSCON_COLOR_YELLOW] = {170, 85, 0},
	[KMSCON_COLOR_BLUE] = {0, 0, 170},
	[KMSCON_COLOR_MAGENTA] = {170, 0, 170},
	[KMSCON_COLOR_CYAN] = {0, 170, 170},
	[KMSCON_COLOR_LIGHT_GREY] = {170, 170, 170},
	[KMSCON_COLOR_DARK_GREY] = {85, 85, 85},
	[KMSCON_COLOR_LIGHT_RED] = {255, 85, 85},
	[KMSCON_COLOR_LIGHT_GREEN] = {85, 255, 85},
	[KMSCON_COLOR_LIGHT_YELLOW] = {255, 255, 85},
	[KMSCON_COLOR_LIGHT_BLUE] = {85, 85, 255},
	[KMSCON_COLOR_LIGHT_MAGENTA] = {255, 85, 255},
	[KMSCON_COLOR_LIGHT_CYAN] = {85, 255, 255},
	[KMSCON_COLOR_WHITE] = {255, 255, 255},

	[KMSCON_COLOR_FOREGROUND] = {170, 170, 170},
	[KMSCON_COLOR_BACKGROUND] = {0, 0, 0},
};

static uint8_t color_palette_nord[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {59, 66, 82},
	[KMSCON_COLOR_RED] = {191, 97, 106},
	[KMSCON_COLOR_GREEN] = {163, 190, 140},
	[KMSCON_COLOR_YELLOW] = {235, 203, 139},
	[KMSCON_COLOR_BLUE] = {129, 161, 193},
	[KMSCON_COLOR_MAGENTA] = {180, 142, 173},
	[KMSCON_COLOR_CYAN] = {136, 192, 208},
	[KMSCON_COLOR_LIGHT_GREY] = {229, 233, 240},
	[KMSCON_COLOR_DARK_GREY] = {76, 86, 106},
	[KMSCON_COLOR_LIGHT_RED] = {191, 97, 106},
	[KMSCON_COLOR_LIGHT_GREEN] = {163, 190, 140},
	[KMSCON_COLOR_LIGHT_YELLOW] = {235, 203, 139},
	[KMSCON_COLOR_LIGHT_BLUE] = {129, 161, 193},
	[KMSCON_COLOR_LIGHT_MAGENTA] = {180, 142, 173},
	[KMSCON_COLOR_LIGHT_CYAN] = {143, 188, 187},
	[KMSCON_COLOR_WHITE] = {236, 239, 244},

	[KMSCON_COLOR_FOREGROUND] = {216, 222, 233},
	[KMSCON_COLOR_BACKGROUND] = {46, 52, 64},
};

static uint8_t color_palette_solarized[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {7, 54, 66},	     [KMSCON_COLOR_RED] = {220, 50, 47},
	[KMSCON_COLOR_GREEN] = {133, 153, 0},	     [KMSCON_COLOR_YELLOW] = {181, 137, 0},
	[KMSCON_COLOR_BLUE] = {38, 139, 210},	     [KMSCON_COLOR_MAGENTA] = {211, 54, 130},
	[KMSCON_COLOR_CYAN] = {42, 161, 152},	     [KMSCON_COLOR_LIGHT_GREY] = {238, 232, 213},
	[KMSCON_COLOR_DARK_GREY] = {0, 43, 54},	     [KMSCON_COLOR_LIGHT_RED] = {203, 75, 22},
	[KMSCON_COLOR_LIGHT_GREEN] = {88, 110, 117}, [KMSCON_COLOR_LIGHT_YELLOW] = {101, 123, 131},
	[KMSCON_COLOR_LIGHT_BLUE] = {131, 148, 150}, [KMSCON_COLOR_LIGHT_MAGENTA] = {108, 113, 196},
	[KMSCON_COLOR_LIGHT_CYAN] = {147, 161, 161}, [KMSCON_COLOR_WHITE] = {253, 246, 227},

	[KMSCON_COLOR_FOREGROUND] = {238, 232, 213}, [KMSCON_COLOR_BACKGROUND] = {7, 54, 66},
};

static uint8_t color_palette_solarized_black[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {0, 0, 0},
	[KMSCON_COLOR_RED] = {220, 50, 47},
	[KMSCON_COLOR_GREEN] = {133, 153, 0},
	[KMSCON_COLOR_YELLOW] = {181, 137, 0},
	[KMSCON_COLOR_BLUE] = {38, 139, 210},
	[KMSCON_COLOR_MAGENTA] = {211, 54, 130},
	[KMSCON_COLOR_CYAN] = {42, 161, 152},
	[KMSCON_COLOR_LIGHT_GREY] = {238, 232, 213},
	[KMSCON_COLOR_DARK_GREY] = {0, 43, 54},
	[KMSCON_COLOR_LIGHT_RED] = {203, 75, 22},
	[KMSCON_COLOR_LIGHT_GREEN] = {88, 110, 117},
	[KMSCON_COLOR_LIGHT_YELLOW] = {101, 123, 131},
	[KMSCON_COLOR_LIGHT_BLUE] = {131, 148, 150},
	[KMSCON_COLOR_LIGHT_MAGENTA] = {108, 113, 196},
	[KMSCON_COLOR_LIGHT_CYAN] = {147, 161, 161},
	[KMSCON_COLOR_WHITE] = {253, 246, 227},

	[KMSCON_COLOR_FOREGROUND] = {238, 232, 213},
	[KMSCON_COLOR_BACKGROUND] = {0, 0, 0},
};

static uint8_t color_palette_solarized_white[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {7, 54, 66},	     [KMSCON_COLOR_RED] = {220, 50, 47},
	[KMSCON_COLOR_GREEN] = {133, 153, 0},	     [KMSCON_COLOR_YELLOW] = {181, 137, 0},
	[KMSCON_COLOR_BLUE] = {38, 139, 210},	     [KMSCON_COLOR_MAGENTA] = {211, 54, 130},
	[KMSCON_COLOR_CYAN] = {42, 161, 152},	     [KMSCON_COLOR_LIGHT_GREY] = {238, 232, 213},
	[KMSCON_COLOR_DARK_GREY] = {0, 43, 54},	     [KMSCON_COLOR_LIGHT_RED] = {203, 75, 22},
	[KMSCON_COLOR_LIGHT_GREEN] = {88, 110, 117}, [KMSCON_COLOR_LIGHT_YELLOW] = {101, 123, 131},
	[KMSCON_COLOR_LIGHT_BLUE] = {131, 148, 150}, [KMSCON_COLOR_LIGHT_MAGENTA] = {108, 113, 196},
	[KMSCON_COLOR_LIGHT_CYAN] = {147, 161, 161}, [KMSCON_COLOR_WHITE] = {253, 246, 227},

	[KMSCON_COLOR_FOREGROUND] = {7, 54, 66},     [KMSCON_COLOR_BACKGROUND] = {238, 232, 213},
};

static uint8_t color_palette_soft_black[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {0x3f, 0x3f, 0x3f},
	[KMSCON_COLOR_RED] = {0x70, 0x50, 0x50},
	[KMSCON_COLOR_GREEN] = {0x60, 0xb4, 0x8a},
	[KMSCON_COLOR_YELLOW] = {0xdf, 0xaf, 0x8f},
	[KMSCON_COLOR_BLUE] = {0x9a, 0xb8, 0xd7},
	[KMSCON_COLOR_MAGENTA] = {0xdc, 0x8c, 0xc3},
	[KMSCON_COLOR_CYAN] = {0x8c, 0xd0, 0xd3},
	[KMSCON_COLOR_LIGHT_GREY] = {0xff, 0xff, 0xff},
	[KMSCON_COLOR_DARK_GREY] = {0x70, 0x90, 0x80},
	[KMSCON_COLOR_LIGHT_RED] = {0xdc, 0xa3, 0xa3},
	[KMSCON_COLOR_LIGHT_GREEN] = {0x72, 0xd5, 0xa3},
	[KMSCON_COLOR_LIGHT_YELLOW] = {0xf0, 0xdf, 0xaf},
	[KMSCON_COLOR_LIGHT_BLUE] = {0x94, 0xbf, 0xf3},
	[KMSCON_COLOR_LIGHT_MAGENTA] = {0xec, 0x93, 0xd3},
	[KMSCON_COLOR_LIGHT_CYAN] = {0x93, 0xe0, 0xe3},
	[KMSCON_COLOR_WHITE] = {0xdc, 0xdc, 0xcc},

	[KMSCON_COLOR_FOREGROUND] = {0xdc, 0xdc, 0xcc},
	[KMSCON_COLOR_BACKGROUND] = {0x2c, 0x2c, 0x2c},
};

static uint8_t color_palette_base16_dark[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {0x00, 0x00, 0x00},
	[KMSCON_COLOR_RED] = {0xab, 0x46, 0x42},
	[KMSCON_COLOR_GREEN] = {0xa1, 0xb5, 0x6c},
	[KMSCON_COLOR_YELLOW] = {0xf7, 0xca, 0x88},
	[KMSCON_COLOR_BLUE] = {0x7c, 0xaf, 0xc2},
	[KMSCON_COLOR_MAGENTA] = {0xba, 0x8b, 0xaf},
	[KMSCON_COLOR_CYAN] = {0x86, 0xc1, 0xb9},
	[KMSCON_COLOR_LIGHT_GREY] = {0xaa, 0xaa, 0xaa},
	[KMSCON_COLOR_DARK_GREY] = {0x55, 0x55, 0x55},
	[KMSCON_COLOR_LIGHT_RED] = {0xab, 0x46, 0x42},
	[KMSCON_COLOR_LIGHT_GREEN] = {0xa1, 0xb5, 0x6c},
	[KMSCON_COLOR_LIGHT_YELLOW] = {0xf7, 0xca, 0x88},
	[KMSCON_COLOR_LIGHT_BLUE] = {0x7c, 0xaf, 0xc2},
	[KMSCON_COLOR_LIGHT_MAGENTA] = {0xba, 0x8b, 0xaf},
	[KMSCON_COLOR_LIGHT_CYAN] = {0x86, 0xc1, 0xb9},
	[KMSCON_COLOR_WHITE] = {0xff, 0xff, 0xff},

	[KMSCON_COLOR_FOREGROUND] = {0xd8, 0xd8, 0xd8},
	[KMSCON_COLOR_BACKGROUND] = {0x18, 0x18, 0x18},
};

static uint8_t color_palette_base16_light[KMSCON_COLOR_NUM][3] = {
	[KMSCON_COLOR_BLACK] = {0x00, 0x00, 0x00},
	[KMSCON_COLOR_RED] = {0xab, 0x46, 0x42},
	[KMSCON_COLOR_GREEN] = {0xa1, 0xb5, 0x6c},
	[KMSCON_COLOR_YELLOW] = {0xf7, 0xca, 0x88},
	[KMSCON_COLOR_BLUE] = {0x7c, 0xaf, 0xc2},
	[KMSCON_COLOR_MAGENTA] = {0xba, 0x8b, 0xaf},
	[KMSCON_COLOR_CYAN] = {0x86, 0xc1, 0xb9},
	[KMSCON_COLOR_LIGHT_GREY] = {0xaa, 0xaa, 0xaa},
	[KMSCON_COLOR_DARK_GREY] = {0x55, 0x55, 0x55},
	[KMSCON_COLOR_LIGHT_RED] = {0xab, 0x46, 0x42},
	[KMSCON_COLOR_LIGHT_GREEN] = {0xa1, 0xb5, 0x6c},
	[KMSCON_COLOR_LIGHT_YELLOW] = {0xf7, 0xca, 0x88},
	[KMSCON_COLOR_LIGHT_BLUE] = {0x7c, 0xaf, 0xc2},
	[KMSCON_COLOR_LIGHT_MAGENTA] = {0xba, 0x8b, 0xaf},
	[KMSCON_COLOR_LIGHT_CYAN] = {0x86, 0xc1, 0xb9},
	[KMSCON_COLOR_WHITE] = {0xff, 0xff, 0xff},

	[KMSCON_COLOR_FOREGROUND] = {0x18, 0x18, 0x18},
	[KMSCON_COLOR_BACKGROUND] = {0xd8, 0xd8, 0xd8},
};

static uint8_t (*get_palette(const char *name, const uint8_t (*custom)[3]))[3]
{
	if (!name)
		return color_palette_legacy;

	if (!strcmp(name, "custom") && custom)
		return (uint8_t (*)[3])custom;
	if (!strcmp(name, "nord"))
		return color_palette_nord;
	if (!strcmp(name, "solarized"))
		return color_palette_solarized;
	if (!strcmp(name, "solarized-black"))
		return color_palette_solarized_black;
	if (!strcmp(name, "solarized-white"))
		return color_palette_solarized_white;
	if (!strcmp(name, "soft-black"))
		return color_palette_soft_black;
	if (!strcmp(name, "base16-dark"))
		return color_palette_base16_dark;
	if (!strcmp(name, "base16-light"))
		return color_palette_base16_light;
	if (!strcmp(name, "vga"))
		return color_palette_vga;
	if (!strcmp(name, "legacy"))
		return color_palette_legacy;

	return color_palette_legacy;
}

/* effects */

static void write_pty_cb(GhosttyTerminal term, void *data, const uint8_t *u8, size_t len)
{
	struct kmscon_vte *vte = data;

	if (vte->write_cb)
		vte->write_cb((const char *)u8, len, vte->data);
}

static void bell_cb(GhosttyTerminal term, void *data)
{
	struct kmscon_vte *vte = data;

	if (vte->bell_cb)
		vte->bell_cb(vte->data);
}

static GhosttyString xtversion_cb(GhosttyTerminal term, void *data)
{
	static const char version[] = "kmscon " BUILD_VERSION;

	return (GhosttyString){.ptr = (const uint8_t *)version, .len = sizeof(version) - 1};
}

static GhosttyString enquiry_cb(GhosttyTerminal term, void *data)
{
	return (GhosttyString){.ptr = NULL, .len = 0};
}

static bool device_attributes_cb(GhosttyTerminal term, void *data, GhosttyDeviceAttributes *out)
{
	*out = (GhosttyDeviceAttributes){0};

	/* We quack as a VT220 with color text, just like ghostty does. DCS
	 * sequences are not supported, so we don't claim a higher level. */
	out->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
	out->primary.features[0] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
	out->primary.num_features = 1;

	out->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;

	return true;
}

static bool size_cb(GhosttyTerminal term, void *data, GhosttySizeReportSize *out)
{
	struct kmscon_vte *vte = data;

	out->columns = vte->cols;
	out->rows = vte->rows_num;
	out->cell_width = vte->cell_width;
	out->cell_height = vte->cell_height;
	return true;
}

/* The console has no light or dark mode of its own, so we derive the scheme
 * from the background of the configured palette. */
static GhosttyColorScheme get_color_scheme(struct kmscon_vte *vte)
{
	GhosttyColorRgb bg = {0, 0, 0};

	ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND, &bg);
	return ghostty_color_perceived_luminance(&bg) < 0.5 ? GHOSTTY_COLOR_SCHEME_DARK
							    : GHOSTTY_COLOR_SCHEME_LIGHT;
}

/* OSC 52 and the iTerm2 clipboard commands end up here, already decoded and
 * with the protocol details normalized away. Reads are never forwarded. */
static GhosttyClipboardWriteResult clipboard_write_cb(GhosttyTerminal term, void *data,
						      const GhosttyClipboardWrite *write)
{
	struct kmscon_vte *vte = data;
	const GhosttyClipboardContent *content = NULL;
	size_t i;

	if (!vte->copy_cb)
		return GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;

	/* A clipboard write is a sized struct, so a library that is older than
	 * the headers we were built against may not have all of our fields. */
	if (write->size < sizeof(*write))
		return GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;

	/* The console has a single copy buffer, so every destination that an
	 * application may ask for ends up in the same place. */
	if (!write->contents_len) {
		vte->copy_cb(NULL, 0, vte->data);
		return GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
	}

	for (i = 0; i < write->contents_len; ++i) {
		const GhosttyString *mime = &write->contents[i].mime;

		if (mime->len >= CLIPBOARD_TEXT_LEN &&
		    !memcmp(mime->ptr, CLIPBOARD_TEXT, CLIPBOARD_TEXT_LEN)) {
			content = &write->contents[i];
			break;
		}
	}
	if (!content)
		return GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;

	vte->copy_cb((const char *)content->data.ptr, content->data.len, vte->data);
	return GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
}

static bool color_scheme_cb(GhosttyTerminal term, void *data, GhosttyColorScheme *out)
{
	struct kmscon_vte *vte = data;

	*out = get_color_scheme(vte);
	return true;
}

/*
 * kmscon supports a few non-standard OSC commands ("setBackground" and
 * "setForeground") which libghostty-vt does not know about. Sniff the byte
 * stream for OSC payloads and report them while still feeding everything to
 * the terminal itself.
 */
static void osc_sniff(struct kmscon_vte *vte, const char *u8, size_t len)
{
	size_t i;
	char c;

	if (!vte->osc_cb)
		return;

	for (i = 0; i < len; ++i) {
		c = u8[i];

		switch (vte->osc_state) {
		case OSC_NONE:
			if (c == 0x1b)
				vte->osc_state = OSC_ESC;
			break;
		case OSC_ESC:
			if (c == ']') {
				vte->osc_state = OSC_BODY;
				vte->osc_len = 0;
			} else if (c == 0x1b) {
				vte->osc_state = OSC_ESC;
			} else {
				vte->osc_state = OSC_NONE;
			}
			break;
		case OSC_BODY:
			if (c == 0x07) {
				vte->osc_buf[vte->osc_len] = 0;
				vte->osc_cb(vte->osc_buf, vte->osc_len, vte->data);
				vte->osc_state = OSC_NONE;
			} else if (c == 0x1b) {
				vte->osc_state = OSC_TERM;
			} else if (c == 0x18 || c == 0x1a) {
				vte->osc_state = OSC_NONE;
			} else if (vte->osc_len + 1 < sizeof(vte->osc_buf)) {
				vte->osc_buf[vte->osc_len++] = c;
			} else {
				vte->osc_state = OSC_NONE;
			}
			break;
		case OSC_TERM:
			if (c == '\\') {
				vte->osc_buf[vte->osc_len] = 0;
				vte->osc_cb(vte->osc_buf, vte->osc_len, vte->data);
				vte->osc_state = OSC_NONE;
			} else if (c == ']') {
				vte->osc_state = OSC_BODY;
				vte->osc_len = 0;
			} else {
				vte->osc_state = OSC_NONE;
			}
			break;
		}
	}
}

/* Send @buf to the application, used for the reports that we generate
 * ourselves instead of letting the terminal answer a query. */
static void write_report(struct kmscon_vte *vte, const char *buf, size_t len)
{
	if (len && vte->write_cb)
		vte->write_cb(buf, len, vte->data);
}

static void report_color_scheme(struct kmscon_vte *vte)
{
	char buf[ENCODE_MAX];
	size_t written = 0;
	bool enabled = false;

	ghostty_terminal_mode_get(vte->term, GHOSTTY_MODE_COLOR_SCHEME_REPORT, &enabled);
	if (!enabled)
		return;

	if (ghostty_color_scheme_report_encode(get_color_scheme(vte), buf, sizeof(buf), &written) ==
	    GHOSTTY_SUCCESS)
		write_report(vte, buf, written);
}

static void update_mouse_mode(struct kmscon_vte *vte)
{
	bool tracking = false;

	ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &tracking);
	if (tracking == vte->mouse_tracking)
		return;

	vte->mouse_tracking = tracking;
	if (vte->mouse_mode_cb)
		vte->mouse_mode_cb(tracking, vte->data);
}

int kmscon_vte_new(struct kmscon_vte **out, unsigned int cols, unsigned int rows,
		   unsigned int max_scrollback, void *data)
{
	struct kmscon_vte *vte;
	GhosttyTerminalOptions opts;
	int ret;

	if (!out || !cols || !rows)
		return -EINVAL;

	vte = malloc(sizeof(*vte));
	if (!vte)
		return -ENOMEM;
	memset(vte, 0, sizeof(*vte));
	vte->data = data;
	vte->cols = cols;
	vte->rows_num = rows;
	vte->cell_width = 1;
	vte->cell_height = 1;
	vte->min_contrast = 1.0;

	opts = (GhosttyTerminalOptions){
		.cols = cols,
		.rows = rows,
		.max_scrollback = max_scrollback,
	};
	if (ghostty_terminal_new(NULL, &vte->term, opts) != GHOSTTY_SUCCESS) {
		log_error("cannot create terminal");
		ret = -ENOMEM;
		goto err_free;
	}

	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_USERDATA, vte);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_WRITE_PTY, (const void *)write_pty_cb);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_BELL, (const void *)bell_cb);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_ENQUIRY, (const void *)enquiry_cb);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_XTVERSION, (const void *)xtversion_cb);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_SIZE, (const void *)size_cb);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
			     (const void *)device_attributes_cb);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
			     (const void *)color_scheme_cb);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
			     (const void *)clipboard_write_cb);

	if (ghostty_render_state_new(NULL, &vte->render) != GHOSTTY_SUCCESS) {
		ret = -ENOMEM;
		goto err_term;
	}
	if (ghostty_render_state_row_iterator_new(NULL, &vte->rows) != GHOSTTY_SUCCESS) {
		ret = -ENOMEM;
		goto err_render;
	}
	if (ghostty_render_state_row_cells_new(NULL, &vte->row_cells) != GHOSTTY_SUCCESS) {
		ret = -ENOMEM;
		goto err_rows;
	}
	if (ghostty_key_encoder_new(NULL, &vte->key_enc) != GHOSTTY_SUCCESS) {
		ret = -ENOMEM;
		goto err_cells;
	}
	if (ghostty_key_event_new(NULL, &vte->key_ev) != GHOSTTY_SUCCESS) {
		ret = -ENOMEM;
		goto err_key_enc;
	}
	if (ghostty_mouse_encoder_new(NULL, &vte->mouse_enc) != GHOSTTY_SUCCESS) {
		ret = -ENOMEM;
		goto err_key_ev;
	}
	if (ghostty_mouse_event_new(NULL, &vte->mouse_ev) != GHOSTTY_SUCCESS) {
		ret = -ENOMEM;
		goto err_mouse_enc;
	}

	kmscon_vte_set_palette(vte, NULL, NULL);

	log_debug("new vte object %p", vte);
	*out = vte;
	return 0;

err_mouse_enc:
	ghostty_mouse_encoder_free(vte->mouse_enc);
err_key_ev:
	ghostty_key_event_free(vte->key_ev);
err_key_enc:
	ghostty_key_encoder_free(vte->key_enc);
err_cells:
	ghostty_render_state_row_cells_free(vte->row_cells);
err_rows:
	ghostty_render_state_row_iterator_free(vte->rows);
err_render:
	ghostty_render_state_free(vte->render);
err_term:
	ghostty_terminal_free(vte->term);
err_free:
	free(vte);
	return ret;
}

void kmscon_vte_free(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	log_debug("free vte object %p", vte);

	ghostty_tracked_grid_ref_free(vte->sel_anchor);
	ghostty_mouse_event_free(vte->mouse_ev);
	ghostty_mouse_encoder_free(vte->mouse_enc);
	ghostty_key_event_free(vte->key_ev);
	ghostty_key_encoder_free(vte->key_enc);
	ghostty_render_state_row_cells_free(vte->row_cells);
	ghostty_render_state_row_iterator_free(vte->rows);
	ghostty_render_state_free(vte->render);
	ghostty_terminal_free(vte->term);
	free(vte->graphemes);
	free(vte->cells);
	free(vte);
}

void kmscon_vte_set_write_cb(struct kmscon_vte *vte, kmscon_vte_write_cb cb)
{
	if (vte)
		vte->write_cb = cb;
}

void kmscon_vte_set_bell_cb(struct kmscon_vte *vte, kmscon_vte_bell_cb cb)
{
	if (vte)
		vte->bell_cb = cb;
}

void kmscon_vte_set_osc_cb(struct kmscon_vte *vte, kmscon_vte_osc_cb cb)
{
	if (vte)
		vte->osc_cb = cb;
}

void kmscon_vte_set_mouse_mode_cb(struct kmscon_vte *vte, kmscon_vte_mouse_mode_cb cb)
{
	if (vte)
		vte->mouse_mode_cb = cb;
}

void kmscon_vte_set_copy_cb(struct kmscon_vte *vte, kmscon_vte_copy_cb cb)
{
	if (vte)
		vte->copy_cb = cb;
}

int kmscon_vte_set_palette(struct kmscon_vte *vte, const char *name, const uint8_t (*custom)[3])
{
	GhosttyColorRgb palette[256];
	GhosttyColorRgb fg, bg;
	uint8_t (*pal)[3];
	unsigned int i;

	if (!vte)
		return -EINVAL;

	pal = get_palette(name, custom);

	ghostty_color_palette_default(palette);
	for (i = 0; i < KMSCON_COLOR_FOREGROUND; ++i) {
		palette[i].r = pal[i][0];
		palette[i].g = pal[i][1];
		palette[i].b = pal[i][2];
	}

	fg.r = pal[KMSCON_COLOR_FOREGROUND][0];
	fg.g = pal[KMSCON_COLOR_FOREGROUND][1];
	fg.b = pal[KMSCON_COLOR_FOREGROUND][2];
	bg.r = pal[KMSCON_COLOR_BACKGROUND][0];
	bg.g = pal[KMSCON_COLOR_BACKGROUND][1];
	bg.b = pal[KMSCON_COLOR_BACKGROUND][2];

	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE, palette);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND, &fg);
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND, &bg);

	report_color_scheme(vte);
	return 0;
}

int kmscon_vte_parse_color(const char *value, uint8_t out[3])
{
	GhosttyColorRgb rgb;

	if (!value || !out)
		return -EINVAL;

	if (ghostty_color_parse(value, strlen(value), &rgb) != GHOSTTY_SUCCESS)
		return -EINVAL;

	out[0] = rgb.r;
	out[1] = rgb.g;
	out[2] = rgb.b;
	return 0;
}

void kmscon_vte_set_backspace_sends_delete(struct kmscon_vte *vte, bool set)
{
	if (vte)
		vte->backspace_sends_delete = set;
}

int kmscon_vte_set_cursor_shape(struct kmscon_vte *vte, const char *name)
{
	GhosttyTerminalCursorStyle style;

	if (!vte)
		return -EINVAL;

	if (!name || !strcmp(name, "block"))
		style = GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK;
	else if (!strcmp(name, "underline"))
		style = GHOSTTY_TERMINAL_CURSOR_STYLE_UNDERLINE;
	else if (!strcmp(name, "bar"))
		style = GHOSTTY_TERMINAL_CURSOR_STYLE_BAR;
	else if (!strcmp(name, "hollow"))
		style = GHOSTTY_TERMINAL_CURSOR_STYLE_BLOCK_HOLLOW;
	else
		return -EINVAL;

	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_STYLE, &style);
	return 0;
}

void kmscon_vte_set_cursor_blink(struct kmscon_vte *vte, bool blink)
{
	if (!vte)
		return;

	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_DEFAULT_CURSOR_BLINK, &blink);
}

void kmscon_vte_set_cursor_color(struct kmscon_vte *vte, const uint8_t rgb[3])
{
	GhosttyColorRgb color;

	if (!vte)
		return;

	if (!rgb) {
		ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, NULL);
		return;
	}

	color.r = rgb[0];
	color.g = rgb[1];
	color.b = rgb[2];
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR, &color);
}

void kmscon_vte_set_min_contrast(struct kmscon_vte *vte, double ratio)
{
	if (!vte)
		return;

	/* WCAG contrast ratios run from 1.0, that is no contrast at all, to
	 * 21.0 for black on white. Anything at or below 1.0 disables this. */
	if (ratio > 21.0)
		ratio = 21.0;

	vte->min_contrast = ratio;
}

void kmscon_vte_set_scrollbar(struct kmscon_vte *vte, bool set)
{
	if (vte)
		vte->scrollbar = set;
}

void kmscon_vte_input(struct kmscon_vte *vte, const char *u8, size_t len)
{
	if (!vte || !u8 || !len)
		return;

	osc_sniff(vte, u8, len);
	ghostty_terminal_vt_write(vte->term, (const uint8_t *)u8, len);
	update_mouse_mode(vte);

	if (!vte->vt_error_logged) {
		bool error = false;

		ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_VT_PROCESSING_ERROR, &error);
		if (error) {
			log_warning("VT processing hit an error, some output may be missing");
			vte->vt_error_logged = true;
		}
	}
}

void kmscon_vte_set_focus(struct kmscon_vte *vte, bool focused)
{
	char buf[ENCODE_MAX];
	size_t written = 0;
	bool enabled = false;

	if (!vte || vte->focused == focused)
		return;

	vte->focused = focused;

	ghostty_terminal_mode_get(vte->term, GHOSTTY_MODE_FOCUS_EVENT, &enabled);
	if (!enabled)
		return;

	if (ghostty_focus_encode(focused ? GHOSTTY_FOCUS_GAINED : GHOSTTY_FOCUS_LOST, buf,
				 sizeof(buf), &written) == GHOSTTY_SUCCESS)
		write_report(vte, buf, written);
}

void kmscon_vte_compress_scrollback(struct kmscon_vte *vte)
{
	GhosttyTerminalCompressionResult res;

	if (!vte)
		return;

	if (ghostty_terminal_compress(vte->term, GHOSTTY_TERMINAL_COMPRESSION_MODE_FULL, &res) !=
	    GHOSTTY_SUCCESS)
		return;

	if (res == GHOSTTY_TERMINAL_COMPRESSION_RESULT_UNSUPPORTED)
		log_debug("scrollback compression is not supported here");
}

void kmscon_vte_hard_reset(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	ghostty_terminal_reset(vte->term);
	vte->osc_state = OSC_NONE;
	vte->osc_len = 0;
	update_mouse_mode(vte);
}

void kmscon_vte_paste(struct kmscon_vte *vte, const char *u8, size_t len)
{
	bool bracketed = false;
	char stack_buf[512];
	char *buf = stack_buf;
	size_t written = 0;
	size_t buf_len;

	if (!vte || !u8 || !len || !vte->write_cb)
		return;

	if (!ghostty_paste_is_safe(u8, len))
		log_debug("pasting data with unsafe content");

	ghostty_terminal_mode_get(vte->term, GHOSTTY_MODE_BRACKETED_PASTE, &bracketed);

	buf_len = sizeof(stack_buf);
	if (ghostty_paste_encode((char *)u8, len, bracketed, buf, buf_len, &written) ==
	    GHOSTTY_OUT_OF_SPACE) {
		buf_len = written;
		buf = malloc(buf_len);
		if (!buf)
			return;
		if (ghostty_paste_encode((char *)u8, len, bracketed, buf, buf_len, &written) !=
		    GHOSTTY_SUCCESS) {
			free(buf);
			return;
		}
	}

	vte->write_cb(buf, written, vte->data);
	if (buf != stack_buf)
		free(buf);
}

int kmscon_vte_resize(struct kmscon_vte *vte, unsigned int cols, unsigned int rows,
		      unsigned int cell_width, unsigned int cell_height)
{
	if (!vte || !cols || !rows)
		return -EINVAL;

	vte->cell_width = cell_width ? cell_width : 1;
	vte->cell_height = cell_height ? cell_height : 1;

	if (ghostty_terminal_resize(vte->term, cols, rows, vte->cell_width, vte->cell_height) !=
	    GHOSTTY_SUCCESS)
		return -EINVAL;

	vte->cols = cols;
	vte->rows_num = rows;
	return 0;
}

unsigned int kmscon_vte_get_cols(struct kmscon_vte *vte)
{
	return vte ? vte->cols : 0;
}

unsigned int kmscon_vte_get_rows(struct kmscon_vte *vte)
{
	return vte ? vte->rows_num : 0;
}

void kmscon_vte_get_def_attr(struct kmscon_vte *vte, struct kmscon_screen_attr *out)
{
	GhosttyColorRgb fg = {0xff, 0xff, 0xff};
	GhosttyColorRgb bg = {0, 0, 0};

	if (!vte || !out)
		return;

	ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_COLOR_FOREGROUND, &fg);
	ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_COLOR_BACKGROUND, &bg);

	out->fg.r = fg.r;
	out->fg.g = fg.g;
	out->fg.b = fg.b;
	out->bg.r = bg.r;
	out->bg.g = bg.g;
	out->bg.b = bg.b;
}

/* rendering */

static int alloc_cells(struct kmscon_vte *vte, size_t count)
{
	struct kmscon_cell *cells;

	if (vte->cells_count >= count)
		return 0;

	cells = realloc(vte->cells, count * sizeof(*cells));
	if (!cells)
		return -ENOMEM;

	vte->cells = cells;
	vte->cells_count = count;
	return 0;
}

static int alloc_graphemes(struct kmscon_vte *vte, size_t count)
{
	uint32_t *buf;

	if (vte->graphemes_count >= count)
		return 0;

	buf = realloc(vte->graphemes, count * sizeof(*buf));
	if (!buf)
		return -ENOMEM;

	vte->graphemes = buf;
	vte->graphemes_count = count;
	return 0;
}

static struct kmscon_color to_color(GhosttyColorRgb rgb)
{
	return (struct kmscon_color){.r = rgb.r, .g = rgb.g, .b = rgb.b};
}

static void color_dim(struct kmscon_color *out, const struct kmscon_color *fg,
		      const struct kmscon_color *bg)
{
	out->r = bg->r / 2 + fg->r / 2;
	out->g = bg->g / 2 + fg->g / 2;
	out->b = bg->b / 2 + fg->b / 2;
}

/* Force the foreground far enough away from the background for the text to
 * stay readable, no matter which colors the application picked. */
static void apply_min_contrast(struct kmscon_vte *vte, struct kmscon_cell *cell)
{
	GhosttyColorRgb fg = {cell->fg.r, cell->fg.g, cell->fg.b};
	GhosttyColorRgb bg = {cell->bg.r, cell->bg.g, cell->bg.b};

	if (ghostty_color_contrast(&fg, &bg) >= vte->min_contrast)
		return;

	/* Black and white are the extremes of the contrast ratio, so picking
	 * the one that is further away from the background gives us the best
	 * result that is still achievable. */
	if (ghostty_color_perceived_luminance(&bg) < 0.5)
		cell->fg = (struct kmscon_color){0xff, 0xff, 0xff};
	else
		cell->fg = (struct kmscon_color){0, 0, 0};
}

static enum kmscon_cursor_shape to_cursor_shape(GhosttyRenderStateCursorVisualStyle style)
{
	switch (style) {
	case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BAR:
		return KMSCON_CURSOR_BAR;
	case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_UNDERLINE:
		return KMSCON_CURSOR_UNDERLINE;
	case GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK_HOLLOW:
		return KMSCON_CURSOR_BLOCK_HOLLOW;
	default:
		return KMSCON_CURSOR_BLOCK;
	}
}

static void draw_cell(struct kmscon_vte *vte, GhosttyRenderStateRowCells cells,
		      const GhosttyRenderStateColors *colors, bool selected,
		      struct kmscon_cell *out)
{
	GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
	GhosttyColorRgb rgb;
	uint32_t graphemes_len = 0;
	bool has_styling = false;
	bool inverse;

	memset(out, 0, sizeof(*out));
	out->fg = to_color(colors->foreground);
	out->bg = to_color(colors->background);

	ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
					   &graphemes_len);
	if (graphemes_len && !alloc_graphemes(vte, graphemes_len)) {
		ghostty_render_state_row_cells_get(
			cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF, vte->graphemes);
		out->ch = vte->graphemes[0];
	}

	ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_HAS_STYLING,
					   &has_styling);
	if (has_styling)
		ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
						   &style);

	if (ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR,
					       &rgb) == GHOSTTY_SUCCESS)
		out->fg = to_color(rgb);
	if (ghostty_render_state_row_cells_get(cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR,
					       &rgb) == GHOSTTY_SUCCESS)
		out->bg = to_color(rgb);

	out->attr.bold = style.bold;
	out->attr.italic = style.italic;
	out->attr.underline = !!style.underline;
	out->attr.blink = style.blink;

	inverse = style.inverse ^ selected;
	if (inverse) {
		struct kmscon_color tmp = out->fg;

		out->fg = out->bg;
		out->bg = tmp;
	}

	if (style.faint)
		color_dim(&out->fg, &out->fg, &out->bg);
	if (vte->min_contrast > 1.0 && out->ch)
		apply_min_contrast(vte, out);
	if (style.invisible)
		out->fg = out->bg;
}

#define SCROLLBAR_THUMB 0x2588	/* FULL BLOCK */
#define SCROLLBAR_TROUGH 0x2502 /* BOX DRAWINGS LIGHT VERTICAL */

/* Replace the last column with a scroll position indicator. This is only done
 * while the user is looking at the scrollback, so a terminal that is scrolled
 * to the bottom keeps all of its columns. */
static void draw_scrollbar(struct kmscon_vte *vte, const GhosttyRenderStateColors *colors,
			   unsigned int cols, unsigned int rows)
{
	struct kmscon_vte_scrollbar sb;
	uint64_t start, len;
	unsigned int y;
	bool active = true;

	if (cols < 2 || !rows)
		return;

	ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE, &active);
	if (active)
		return;

	kmscon_vte_get_scrollbar(vte, &sb);
	if (!sb.total || sb.len >= sb.total)
		return;

	len = (sb.len * rows + sb.total - 1) / sb.total;
	if (!len)
		len = 1;
	start = sb.offset * rows / sb.total;
	if (start + len > rows)
		start = rows - len;

	for (y = 0; y < rows; ++y) {
		struct kmscon_cell *cell = &vte->cells[y * cols + cols - 1];

		memset(cell, 0, sizeof(*cell));
		cell->fg = to_color(colors->foreground);
		cell->bg = to_color(colors->background);
		cell->ch = (y >= start && y < start + len) ? SCROLLBAR_THUMB : SCROLLBAR_TROUGH;
	}
}

int kmscon_vte_draw(struct kmscon_vte *vte, struct kmscon_vte_screen *out)
{
	GhosttyRenderStateColors colors = GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
	GhosttyRenderStateCursorVisualStyle cursor_style;
	uint16_t cols = 0, rows = 0, cursor_x = 0, cursor_y = 0;
	bool cursor_visible = false, cursor_in_viewport = false, cursor_blinking = false;
	unsigned int x, y;
	int ret;

	if (!vte || !out)
		return -EINVAL;

	if (ghostty_render_state_update(vte->render, vte->term) != GHOSTTY_SUCCESS)
		return -EFAULT;

	ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_COLS, &cols);
	ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_ROWS, &rows);
	ghostty_render_state_colors_get(vte->render, &colors);

	ret = alloc_cells(vte, (size_t)cols * rows);
	if (ret)
		return ret;
	memset(vte->cells, 0, (size_t)cols * rows * sizeof(*vte->cells));

	if (ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR,
				     &vte->rows) != GHOSTTY_SUCCESS)
		return -EFAULT;

	for (y = 0; y < rows && ghostty_render_state_row_iterator_next(vte->rows); ++y) {
		GhosttyRenderStateRowSelection sel =
			GHOSTTY_INIT_SIZED(GhosttyRenderStateRowSelection);
		bool has_sel;

		has_sel = ghostty_render_state_row_get(vte->rows,
						       GHOSTTY_RENDER_STATE_ROW_DATA_SELECTION,
						       &sel) == GHOSTTY_SUCCESS;

		if (ghostty_render_state_row_get(vte->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS,
						 &vte->row_cells) != GHOSTTY_SUCCESS)
			continue;

		for (x = 0; x < cols && ghostty_render_state_row_cells_next(vte->row_cells); ++x) {
			bool selected = has_sel && x >= sel.start_x && x <= sel.end_x;

			draw_cell(vte, vte->row_cells, &colors, selected,
				  &vte->cells[y * cols + x]);
		}
	}

	if (vte->scrollbar)
		draw_scrollbar(vte, &colors, cols, rows);

	ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE,
				 &cursor_visible);
	ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
				 &cursor_in_viewport);
	ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_BLINKING,
				 &cursor_blinking);
	if (cursor_in_viewport) {
		ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X,
					 &cursor_x);
		ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y,
					 &cursor_y);
	}
	if (ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_CURSOR_VISUAL_STYLE,
				     &cursor_style) != GHOSTTY_SUCCESS)
		cursor_style = GHOSTTY_RENDER_STATE_CURSOR_VISUAL_STYLE_BLOCK;

	out->cells = vte->cells;
	out->cols = cols;
	out->rows = rows;
	out->cursor_x = cursor_x;
	out->cursor_y = cursor_y;
	out->cursor_visible = cursor_visible && cursor_in_viewport;
	out->cursor_blinks = cursor_blinking;
	out->cursor_shape = to_cursor_shape(cursor_style);

	out->cursor_has_color = false;
	ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR_HAS_VALUE,
				 &out->cursor_has_color);
	if (out->cursor_has_color) {
		GhosttyColorRgb rgb;

		if (ghostty_render_state_get(vte->render, GHOSTTY_RENDER_STATE_DATA_COLOR_CURSOR,
					     &rgb) == GHOSTTY_SUCCESS)
			out->cursor_color = to_color(rgb);
		else
			out->cursor_has_color = false;
	}
	return 0;
}

void kmscon_vte_get_scrollbar(struct kmscon_vte *vte, struct kmscon_vte_scrollbar *out)
{
	GhosttyTerminalScrollbar sb = {0};

	if (!vte || !out)
		return;

	ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_SCROLLBAR, &sb);
	out->total = sb.total;
	out->offset = sb.offset;
	out->len = sb.len;
}

/* scrollback */

static void scroll_delta(struct kmscon_vte *vte, intptr_t delta)
{
	GhosttyTerminalScrollViewport behavior = {
		.tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
		.value = {.delta = delta},
	};

	if (!vte || !delta)
		return;

	ghostty_terminal_scroll_viewport(vte->term, behavior);
}

void kmscon_vte_sb_up(struct kmscon_vte *vte, unsigned int num)
{
	scroll_delta(vte, -(intptr_t)num);
}

void kmscon_vte_sb_down(struct kmscon_vte *vte, unsigned int num)
{
	scroll_delta(vte, (intptr_t)num);
}

void kmscon_vte_sb_page_up(struct kmscon_vte *vte, unsigned int num)
{
	if (vte)
		scroll_delta(vte, -(intptr_t)(num * vte->rows_num));
}

void kmscon_vte_sb_page_down(struct kmscon_vte *vte, unsigned int num)
{
	if (vte)
		scroll_delta(vte, (intptr_t)(num * vte->rows_num));
}

/* Rows are marked as prompts by the OSC 133 sequences that a shell with
 * kmscon's shell integration emits. */
static bool row_is_prompt(struct kmscon_vte *vte, uint64_t y)
{
	GhosttyGridRef ref = GHOSTTY_INIT_SIZED(GhosttyGridRef);
	GhosttyRowSemanticPrompt prompt;
	GhosttyRow row;
	GhosttyPoint point = {
		.tag = GHOSTTY_POINT_TAG_SCREEN,
		.value = {.coordinate = {.x = 0, .y = y}},
	};

	if (ghostty_terminal_grid_ref(vte->term, point, &ref) != GHOSTTY_SUCCESS)
		return false;
	if (ghostty_grid_ref_row(&ref, &row) != GHOSTTY_SUCCESS)
		return false;
	if (ghostty_row_get(row, GHOSTTY_ROW_DATA_SEMANTIC_PROMPT, &prompt) != GHOSTTY_SUCCESS)
		return false;

	return prompt == GHOSTTY_ROW_SEMANTIC_PROMPT;
}

int kmscon_vte_jump_to_prompt(struct kmscon_vte *vte, int delta)
{
	struct kmscon_vte_scrollbar sb;
	int step = delta < 0 ? -1 : 1;
	unsigned int todo;
	int64_t row;

	if (!vte || !delta)
		return -EINVAL;

	kmscon_vte_get_scrollbar(vte, &sb);
	if (!sb.total)
		return -ENOENT;

	todo = delta < 0 ? -delta : delta;
	row = (int64_t)sb.offset;

	while (todo) {
		row += step;
		if (row < 0 || (uint64_t)row >= sb.total)
			return -ENOENT;
		if (row_is_prompt(vte, (uint64_t)row))
			--todo;
	}

	ghostty_terminal_scroll_viewport(vte->term, (GhosttyTerminalScrollViewport){
							    .tag = GHOSTTY_SCROLL_VIEWPORT_ROW,
							    .value = {.row = (size_t)row},
						    });
	return 0;
}

void kmscon_vte_sb_reset(struct kmscon_vte *vte)
{
	GhosttyTerminalScrollViewport behavior = {.tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM};

	if (!vte)
		return;

	ghostty_terminal_scroll_viewport(vte->term, behavior);
}

/* selection */

static bool viewport_ref(struct kmscon_vte *vte, unsigned int x, unsigned int y,
			 GhosttyGridRef *out)
{
	GhosttyPoint point = {
		.tag = GHOSTTY_POINT_TAG_VIEWPORT,
		.value = {.coordinate = {.x = x, .y = y}},
	};

	*out = (GhosttyGridRef)GHOSTTY_INIT_SIZED(GhosttyGridRef);
	return ghostty_terminal_grid_ref(vte->term, point, out) == GHOSTTY_SUCCESS;
}

static void set_selection(struct kmscon_vte *vte, const GhosttySelection *sel)
{
	ghostty_terminal_set(vte->term, GHOSTTY_TERMINAL_OPT_SELECTION, sel);
}

void kmscon_vte_selection_reset(struct kmscon_vte *vte)
{
	if (!vte)
		return;

	if (vte->sel_anchor) {
		ghostty_tracked_grid_ref_free(vte->sel_anchor);
		vte->sel_anchor = NULL;
	}
	set_selection(vte, NULL);
}

void kmscon_vte_selection_start(struct kmscon_vte *vte, unsigned int x, unsigned int y)
{
	GhosttyPoint point = {
		.tag = GHOSTTY_POINT_TAG_VIEWPORT,
		.value = {.coordinate = {.x = x, .y = y}},
	};

	if (!vte)
		return;

	kmscon_vte_selection_reset(vte);
	if (ghostty_terminal_grid_ref_track(vte->term, point, &vte->sel_anchor) != GHOSTTY_SUCCESS)
		vte->sel_anchor = NULL;
}

void kmscon_vte_selection_target(struct kmscon_vte *vte, unsigned int x, unsigned int y)
{
	GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);

	if (!vte || !vte->sel_anchor)
		return;

	if (!ghostty_tracked_grid_ref_has_value(vte->sel_anchor))
		return;
	if (ghostty_tracked_grid_ref_snapshot(vte->sel_anchor, &sel.start) != GHOSTTY_SUCCESS)
		return;
	if (!viewport_ref(vte, x, y, &sel.end))
		return;

	set_selection(vte, &sel);
}

void kmscon_vte_selection_word(struct kmscon_vte *vte, unsigned int x, unsigned int y)
{
	GhosttyTerminalSelectWordOptions opts =
		GHOSTTY_INIT_SIZED(GhosttyTerminalSelectWordOptions);
	GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);

	if (!vte)
		return;

	kmscon_vte_selection_reset(vte);
	if (!viewport_ref(vte, x, y, &opts.ref))
		return;
	if (ghostty_terminal_select_word(vte->term, &opts, &sel) != GHOSTTY_SUCCESS)
		return;

	set_selection(vte, &sel);
}

void kmscon_vte_selection_output(struct kmscon_vte *vte, unsigned int x, unsigned int y)
{
	GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
	GhosttyGridRef ref;

	if (!vte)
		return;

	kmscon_vte_selection_reset(vte);
	if (!viewport_ref(vte, x, y, &ref))
		return;

	/* Without shell integration the terminal does not know where the
	 * output of a command starts and ends, so this does nothing. */
	if (ghostty_terminal_select_output(vte->term, ref, &sel) != GHOSTTY_SUCCESS)
		return;

	set_selection(vte, &sel);
}

int kmscon_vte_selection_copy(struct kmscon_vte *vte, char **out)
{
	GhosttyTerminalSelectionFormatOptions opts =
		GHOSTTY_INIT_SIZED(GhosttyTerminalSelectionFormatOptions);
	GhosttySelection sel = GHOSTTY_INIT_SIZED(GhosttySelection);
	uint8_t *buf = NULL;
	size_t len = 0;
	char *str;

	if (!vte || !out)
		return -EINVAL;

	*out = NULL;
	if (ghostty_terminal_get(vte->term, GHOSTTY_TERMINAL_DATA_SELECTION, &sel) !=
	    GHOSTTY_SUCCESS)
		return 0;

	opts.emit = GHOSTTY_FORMATTER_FORMAT_PLAIN;
	opts.trim = true;
	opts.selection = &sel;

	if (ghostty_terminal_selection_format_alloc(vte->term, NULL, opts, &buf, &len) !=
	    GHOSTTY_SUCCESS)
		return 0;

	str = malloc(len + 1);
	if (str) {
		memcpy(str, buf, len);
		str[len] = 0;
		*out = str;
	}
	ghostty_free(NULL, buf, len);

	return str ? (int)len : -ENOMEM;
}

/* keyboard */

/* @keycode is an XKB keycode, the tables below use linux keycodes */
static GhosttyKey keycode_to_key(uint16_t keycode)
{
	if (keycode < INPUT_KEYCODE_OFFSET)
		return GHOSTTY_KEY_UNIDENTIFIED;

	switch (keycode - INPUT_KEYCODE_OFFSET) {
	case KEY_ESC:
		return GHOSTTY_KEY_ESCAPE;
	case KEY_1:
		return GHOSTTY_KEY_DIGIT_1;
	case KEY_2:
		return GHOSTTY_KEY_DIGIT_2;
	case KEY_3:
		return GHOSTTY_KEY_DIGIT_3;
	case KEY_4:
		return GHOSTTY_KEY_DIGIT_4;
	case KEY_5:
		return GHOSTTY_KEY_DIGIT_5;
	case KEY_6:
		return GHOSTTY_KEY_DIGIT_6;
	case KEY_7:
		return GHOSTTY_KEY_DIGIT_7;
	case KEY_8:
		return GHOSTTY_KEY_DIGIT_8;
	case KEY_9:
		return GHOSTTY_KEY_DIGIT_9;
	case KEY_0:
		return GHOSTTY_KEY_DIGIT_0;
	case KEY_MINUS:
		return GHOSTTY_KEY_MINUS;
	case KEY_EQUAL:
		return GHOSTTY_KEY_EQUAL;
	case KEY_BACKSPACE:
		return GHOSTTY_KEY_BACKSPACE;
	case KEY_TAB:
		return GHOSTTY_KEY_TAB;
	case KEY_Q:
		return GHOSTTY_KEY_Q;
	case KEY_W:
		return GHOSTTY_KEY_W;
	case KEY_E:
		return GHOSTTY_KEY_E;
	case KEY_R:
		return GHOSTTY_KEY_R;
	case KEY_T:
		return GHOSTTY_KEY_T;
	case KEY_Y:
		return GHOSTTY_KEY_Y;
	case KEY_U:
		return GHOSTTY_KEY_U;
	case KEY_I:
		return GHOSTTY_KEY_I;
	case KEY_O:
		return GHOSTTY_KEY_O;
	case KEY_P:
		return GHOSTTY_KEY_P;
	case KEY_LEFTBRACE:
		return GHOSTTY_KEY_BRACKET_LEFT;
	case KEY_RIGHTBRACE:
		return GHOSTTY_KEY_BRACKET_RIGHT;
	case KEY_ENTER:
		return GHOSTTY_KEY_ENTER;
	case KEY_LEFTCTRL:
		return GHOSTTY_KEY_CONTROL_LEFT;
	case KEY_A:
		return GHOSTTY_KEY_A;
	case KEY_S:
		return GHOSTTY_KEY_S;
	case KEY_D:
		return GHOSTTY_KEY_D;
	case KEY_F:
		return GHOSTTY_KEY_F;
	case KEY_G:
		return GHOSTTY_KEY_G;
	case KEY_H:
		return GHOSTTY_KEY_H;
	case KEY_J:
		return GHOSTTY_KEY_J;
	case KEY_K:
		return GHOSTTY_KEY_K;
	case KEY_L:
		return GHOSTTY_KEY_L;
	case KEY_SEMICOLON:
		return GHOSTTY_KEY_SEMICOLON;
	case KEY_APOSTROPHE:
		return GHOSTTY_KEY_QUOTE;
	case KEY_GRAVE:
		return GHOSTTY_KEY_BACKQUOTE;
	case KEY_LEFTSHIFT:
		return GHOSTTY_KEY_SHIFT_LEFT;
	case KEY_BACKSLASH:
		return GHOSTTY_KEY_BACKSLASH;
	case KEY_Z:
		return GHOSTTY_KEY_Z;
	case KEY_X:
		return GHOSTTY_KEY_X;
	case KEY_C:
		return GHOSTTY_KEY_C;
	case KEY_V:
		return GHOSTTY_KEY_V;
	case KEY_B:
		return GHOSTTY_KEY_B;
	case KEY_N:
		return GHOSTTY_KEY_N;
	case KEY_M:
		return GHOSTTY_KEY_M;
	case KEY_COMMA:
		return GHOSTTY_KEY_COMMA;
	case KEY_DOT:
		return GHOSTTY_KEY_PERIOD;
	case KEY_SLASH:
		return GHOSTTY_KEY_SLASH;
	case KEY_RIGHTSHIFT:
		return GHOSTTY_KEY_SHIFT_RIGHT;
	case KEY_KPASTERISK:
		return GHOSTTY_KEY_NUMPAD_MULTIPLY;
	case KEY_LEFTALT:
		return GHOSTTY_KEY_ALT_LEFT;
	case KEY_SPACE:
		return GHOSTTY_KEY_SPACE;
	case KEY_CAPSLOCK:
		return GHOSTTY_KEY_CAPS_LOCK;
	case KEY_F1:
		return GHOSTTY_KEY_F1;
	case KEY_F2:
		return GHOSTTY_KEY_F2;
	case KEY_F3:
		return GHOSTTY_KEY_F3;
	case KEY_F4:
		return GHOSTTY_KEY_F4;
	case KEY_F5:
		return GHOSTTY_KEY_F5;
	case KEY_F6:
		return GHOSTTY_KEY_F6;
	case KEY_F7:
		return GHOSTTY_KEY_F7;
	case KEY_F8:
		return GHOSTTY_KEY_F8;
	case KEY_F9:
		return GHOSTTY_KEY_F9;
	case KEY_F10:
		return GHOSTTY_KEY_F10;
	case KEY_NUMLOCK:
		return GHOSTTY_KEY_NUM_LOCK;
	case KEY_SCROLLLOCK:
		return GHOSTTY_KEY_SCROLL_LOCK;
	case KEY_KP7:
		return GHOSTTY_KEY_NUMPAD_7;
	case KEY_KP8:
		return GHOSTTY_KEY_NUMPAD_8;
	case KEY_KP9:
		return GHOSTTY_KEY_NUMPAD_9;
	case KEY_KPMINUS:
		return GHOSTTY_KEY_NUMPAD_SUBTRACT;
	case KEY_KP4:
		return GHOSTTY_KEY_NUMPAD_4;
	case KEY_KP5:
		return GHOSTTY_KEY_NUMPAD_5;
	case KEY_KP6:
		return GHOSTTY_KEY_NUMPAD_6;
	case KEY_KPPLUS:
		return GHOSTTY_KEY_NUMPAD_ADD;
	case KEY_KP1:
		return GHOSTTY_KEY_NUMPAD_1;
	case KEY_KP2:
		return GHOSTTY_KEY_NUMPAD_2;
	case KEY_KP3:
		return GHOSTTY_KEY_NUMPAD_3;
	case KEY_KP0:
		return GHOSTTY_KEY_NUMPAD_0;
	case KEY_KPDOT:
		return GHOSTTY_KEY_NUMPAD_DECIMAL;
	case KEY_ZENKAKUHANKAKU:
		return GHOSTTY_KEY_KANA_MODE;
	case KEY_102ND:
		return GHOSTTY_KEY_INTL_BACKSLASH;
	case KEY_F11:
		return GHOSTTY_KEY_F11;
	case KEY_F12:
		return GHOSTTY_KEY_F12;
	case KEY_RO:
		return GHOSTTY_KEY_INTL_RO;
	case KEY_KATAKANAHIRAGANA:
		return GHOSTTY_KEY_KANA_MODE;
	case KEY_HENKAN:
		return GHOSTTY_KEY_CONVERT;
	case KEY_MUHENKAN:
		return GHOSTTY_KEY_NON_CONVERT;
	case KEY_KPJPCOMMA:
		return GHOSTTY_KEY_NUMPAD_COMMA;
	case KEY_KPENTER:
		return GHOSTTY_KEY_NUMPAD_ENTER;
	case KEY_RIGHTCTRL:
		return GHOSTTY_KEY_CONTROL_RIGHT;
	case KEY_KPSLASH:
		return GHOSTTY_KEY_NUMPAD_DIVIDE;
	case KEY_SYSRQ:
		return GHOSTTY_KEY_PRINT_SCREEN;
	case KEY_RIGHTALT:
		return GHOSTTY_KEY_ALT_RIGHT;
	case KEY_HOME:
		return GHOSTTY_KEY_HOME;
	case KEY_UP:
		return GHOSTTY_KEY_ARROW_UP;
	case KEY_PAGEUP:
		return GHOSTTY_KEY_PAGE_UP;
	case KEY_LEFT:
		return GHOSTTY_KEY_ARROW_LEFT;
	case KEY_RIGHT:
		return GHOSTTY_KEY_ARROW_RIGHT;
	case KEY_END:
		return GHOSTTY_KEY_END;
	case KEY_DOWN:
		return GHOSTTY_KEY_ARROW_DOWN;
	case KEY_PAGEDOWN:
		return GHOSTTY_KEY_PAGE_DOWN;
	case KEY_INSERT:
		return GHOSTTY_KEY_INSERT;
	case KEY_DELETE:
		return GHOSTTY_KEY_DELETE;
	case KEY_MUTE:
		return GHOSTTY_KEY_AUDIO_VOLUME_MUTE;
	case KEY_VOLUMEDOWN:
		return GHOSTTY_KEY_AUDIO_VOLUME_DOWN;
	case KEY_VOLUMEUP:
		return GHOSTTY_KEY_AUDIO_VOLUME_UP;
	case KEY_POWER:
		return GHOSTTY_KEY_POWER;
	case KEY_KPEQUAL:
		return GHOSTTY_KEY_NUMPAD_EQUAL;
	case KEY_PAUSE:
		return GHOSTTY_KEY_PAUSE;
	case KEY_KPCOMMA:
		return GHOSTTY_KEY_NUMPAD_COMMA;
	case KEY_YEN:
		return GHOSTTY_KEY_INTL_YEN;
	case KEY_LEFTMETA:
		return GHOSTTY_KEY_META_LEFT;
	case KEY_RIGHTMETA:
		return GHOSTTY_KEY_META_RIGHT;
	case KEY_COMPOSE:
		return GHOSTTY_KEY_CONTEXT_MENU;
	case KEY_STOP:
		return GHOSTTY_KEY_BROWSER_STOP;
	case KEY_AGAIN:
		return GHOSTTY_KEY_LAUNCH_APP_1;
	case KEY_COPY:
		return GHOSTTY_KEY_COPY;
	case KEY_PASTE:
		return GHOSTTY_KEY_PASTE;
	case KEY_CUT:
		return GHOSTTY_KEY_CUT;
	case KEY_HELP:
		return GHOSTTY_KEY_HELP;
	case KEY_F13:
		return GHOSTTY_KEY_F13;
	case KEY_F14:
		return GHOSTTY_KEY_F14;
	case KEY_F15:
		return GHOSTTY_KEY_F15;
	case KEY_F16:
		return GHOSTTY_KEY_F16;
	case KEY_F17:
		return GHOSTTY_KEY_F17;
	case KEY_F18:
		return GHOSTTY_KEY_F18;
	case KEY_F19:
		return GHOSTTY_KEY_F19;
	case KEY_F20:
		return GHOSTTY_KEY_F20;
	case KEY_F21:
		return GHOSTTY_KEY_F21;
	case KEY_F22:
		return GHOSTTY_KEY_F22;
	case KEY_F23:
		return GHOSTTY_KEY_F23;
	case KEY_F24:
		return GHOSTTY_KEY_F24;
	case KEY_PLAYPAUSE:
		return GHOSTTY_KEY_MEDIA_PLAY_PAUSE;
	case KEY_STOPCD:
		return GHOSTTY_KEY_MEDIA_STOP;
	case KEY_PREVIOUSSONG:
		return GHOSTTY_KEY_MEDIA_TRACK_PREVIOUS;
	case KEY_NEXTSONG:
		return GHOSTTY_KEY_MEDIA_TRACK_NEXT;
	case KEY_EJECTCD:
		return GHOSTTY_KEY_EJECT;
	case KEY_REFRESH:
		return GHOSTTY_KEY_BROWSER_REFRESH;
	case KEY_SLEEP:
		return GHOSTTY_KEY_SLEEP;
	case KEY_WAKEUP:
		return GHOSTTY_KEY_WAKE_UP;
	case KEY_MAIL:
		return GHOSTTY_KEY_LAUNCH_MAIL;
	case KEY_BOOKMARKS:
		return GHOSTTY_KEY_BROWSER_FAVORITES;
	case KEY_COMPUTER:
		return GHOSTTY_KEY_LAUNCH_APP_2;
	case KEY_BACK:
		return GHOSTTY_KEY_BROWSER_BACK;
	case KEY_FORWARD:
		return GHOSTTY_KEY_BROWSER_FORWARD;
	case KEY_MEDIA:
		return GHOSTTY_KEY_MEDIA_SELECT;
	case KEY_HOMEPAGE:
		return GHOSTTY_KEY_BROWSER_HOME;
	case KEY_SEARCH:
		return GHOSTTY_KEY_BROWSER_SEARCH;
	case KEY_FN:
		return GHOSTTY_KEY_FN;
	default:
		return GHOSTTY_KEY_UNIDENTIFIED;
	}
}

static GhosttyMods to_ghostty_mods(unsigned int mods)
{
	GhosttyMods out = 0;

	if (mods & INPUT_SHIFT_MASK)
		out |= GHOSTTY_MODS_SHIFT;
	if (mods & INPUT_LOCK_MASK)
		out |= GHOSTTY_MODS_CAPS_LOCK;
	if (mods & INPUT_CONTROL_MASK)
		out |= GHOSTTY_MODS_CTRL;
	if (mods & INPUT_ALT_MASK)
		out |= GHOSTTY_MODS_ALT;
	if (mods & INPUT_LOGO_MASK)
		out |= GHOSTTY_MODS_SUPER;

	return out;
}

static void sync_key_encoder(struct kmscon_vte *vte)
{
	bool backarrow = false;

	ghostty_key_encoder_setopt_from_terminal(vte->key_enc, vte->term);

	ghostty_terminal_mode_get(vte->term, GHOSTTY_MODE_BACKARROW_KEY_MODE, &backarrow);
	backarrow = backarrow || !vte->backspace_sends_delete;
	ghostty_key_encoder_setopt(vte->key_enc, GHOSTTY_KEY_ENCODER_OPT_BACKARROW_KEY_MODE,
				   &backarrow);
}

bool kmscon_vte_handle_keyboard(struct kmscon_vte *vte, uint16_t keycode, uint32_t ascii,
				uint32_t unicode, unsigned int mods)
{
	char utf8[SHL_UCS4_MAX_LEN];
	char buf[ENCODE_MAX];
	size_t utf8_len = 0;
	size_t written = 0;
	GhosttyKey key;

	if (!vte)
		return false;

	key = keycode_to_key(keycode);
	if (key == GHOSTTY_KEY_UNIDENTIFIED && unicode == INPUT_INVALID)
		return false;

	if (unicode != INPUT_INVALID)
		utf8_len = shl_ucs4_to_utf8(unicode, utf8);

	ghostty_key_event_set_action(vte->key_ev, GHOSTTY_KEY_ACTION_PRESS);
	ghostty_key_event_set_key(vte->key_ev, key);
	ghostty_key_event_set_mods(vte->key_ev, to_ghostty_mods(mods));
	ghostty_key_event_set_consumed_mods(vte->key_ev, 0);
	ghostty_key_event_set_composing(vte->key_ev, false);
	ghostty_key_event_set_utf8(vte->key_ev, utf8, utf8_len);
	ghostty_key_event_set_unshifted_codepoint(vte->key_ev, ascii < 0x80 ? ascii : 0);

	sync_key_encoder(vte);
	if (ghostty_key_encoder_encode(vte->key_enc, vte->key_ev, buf, sizeof(buf), &written) !=
	    GHOSTTY_SUCCESS)
		return false;
	if (!written)
		return false;

	if (vte->write_cb)
		vte->write_cb(buf, written, vte->data);
	return true;
}

/* mouse */

bool kmscon_vte_get_mouse_tracking(struct kmscon_vte *vte)
{
	return vte ? vte->mouse_tracking : false;
}

static GhosttyMouseButton to_ghostty_button(unsigned int button)
{
	switch (button) {
	case 0:
		return GHOSTTY_MOUSE_BUTTON_LEFT;
	case 1:
		return GHOSTTY_MOUSE_BUTTON_MIDDLE;
	case 2:
		return GHOSTTY_MOUSE_BUTTON_RIGHT;
	case 3:
		return GHOSTTY_MOUSE_BUTTON_FOUR;
	case 4:
		return GHOSTTY_MOUSE_BUTTON_FIVE;
	default:
		return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
	}
}

void kmscon_vte_handle_mouse(struct kmscon_vte *vte, int32_t pixel_x, int32_t pixel_y,
			     unsigned int button, enum kmscon_mouse_event event, unsigned int mods)
{
	GhosttyMouseEncoderSize size = GHOSTTY_INIT_SIZED(GhosttyMouseEncoderSize);
	GhosttyMouseAction action;
	char buf[ENCODE_MAX];
	size_t written = 0;

	if (!vte)
		return;

	switch (event) {
	case KMSCON_MOUSE_PRESSED:
		action = GHOSTTY_MOUSE_ACTION_PRESS;
		vte->any_button_pressed = true;
		break;
	case KMSCON_MOUSE_RELEASED:
		action = GHOSTTY_MOUSE_ACTION_RELEASE;
		vte->any_button_pressed = false;
		break;
	default:
		action = GHOSTTY_MOUSE_ACTION_MOTION;
		break;
	}

	size.screen_width = vte->cols * vte->cell_width;
	size.screen_height = vte->rows_num * vte->cell_height;
	size.cell_width = vte->cell_width;
	size.cell_height = vte->cell_height;

	ghostty_mouse_encoder_setopt_from_terminal(vte->mouse_enc, vte->term);
	ghostty_mouse_encoder_setopt(vte->mouse_enc, GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &size);
	ghostty_mouse_encoder_setopt(vte->mouse_enc, GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
				     &vte->any_button_pressed);

	ghostty_mouse_event_set_action(vte->mouse_ev, action);
	if (action == GHOSTTY_MOUSE_ACTION_MOTION && !vte->any_button_pressed)
		ghostty_mouse_event_clear_button(vte->mouse_ev);
	else
		ghostty_mouse_event_set_button(vte->mouse_ev, to_ghostty_button(button));
	ghostty_mouse_event_set_mods(vte->mouse_ev, to_ghostty_mods(mods));
	ghostty_mouse_event_set_position(vte->mouse_ev, (GhosttyMousePosition){
								.x = (float)pixel_x,
								.y = (float)pixel_y,
							});

	if (ghostty_mouse_encoder_encode(vte->mouse_enc, vte->mouse_ev, buf, sizeof(buf),
					 &written) != GHOSTTY_SUCCESS)
		return;
	if (written && vte->write_cb)
		vte->write_cb(buf, written, vte->data);
}
