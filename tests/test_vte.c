/*
 * Tests for the vte layer, that is the libghostty-vt integration.
 */

#include <assert.h>
#include <linux/input-event-codes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "input/input.h"
#include "vte.h"

struct sink {
	char buf[4096];
	size_t len;
	unsigned int bells;
	unsigned int osc_calls;
	char osc[256];
	bool tracking;
};

static void sink_write(const char *u8, size_t len, void *data)
{
	struct sink *sink = data;

	assert(sink->len + len < sizeof(sink->buf));
	memcpy(sink->buf + sink->len, u8, len);
	sink->len += len;
	sink->buf[sink->len] = 0;
}

static void sink_bell(void *data)
{
	struct sink *sink = data;

	sink->bells++;
}

static void sink_osc(const char *osc, size_t len, void *data)
{
	struct sink *sink = data;

	sink->osc_calls++;
	assert(len < sizeof(sink->osc));
	memcpy(sink->osc, osc, len + 1);
}

static void sink_mouse_mode(bool tracking, void *data)
{
	struct sink *sink = data;

	sink->tracking = tracking;
}

static struct kmscon_vte *new_vte(struct sink *sink)
{
	struct kmscon_vte *vte = NULL;
	int ret;

	memset(sink, 0, sizeof(*sink));
	ret = kmscon_vte_new(&vte, 20, 5, 100, sink);
	assert(!ret);
	assert(vte);

	kmscon_vte_set_write_cb(vte, sink_write);
	kmscon_vte_set_bell_cb(vte, sink_bell);
	kmscon_vte_set_osc_cb(vte, sink_osc);
	kmscon_vte_set_mouse_mode_cb(vte, sink_mouse_mode);
	kmscon_vte_resize(vte, 20, 5, 8, 16);
	return vte;
}

static void input_str(struct kmscon_vte *vte, const char *str)
{
	kmscon_vte_input(vte, str, strlen(str));
}

static const struct kmscon_cell *cell_at(struct kmscon_vte_screen *screen, unsigned int x,
					 unsigned int y)
{
	return &screen->cells[x + y * screen->cols];
}

static void test_text_and_cursor(void)
{
	struct kmscon_vte_screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "hello\r\nworld");

	assert(!kmscon_vte_draw(vte, &screen));
	assert(screen.cols == 20 && screen.rows == 5);
	assert(cell_at(&screen, 0, 0)->ch == 'h');
	assert(cell_at(&screen, 4, 0)->ch == 'o');
	assert(cell_at(&screen, 0, 1)->ch == 'w');
	assert(cell_at(&screen, 5, 0)->ch == 0);
	assert(screen.cursor_visible);
	assert(screen.cursor_x == 5 && screen.cursor_y == 1);

	/* DECTCEM hides the cursor */
	input_str(vte, "\033[?25l");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(!screen.cursor_visible);

	kmscon_vte_free(vte);
}

static void test_attributes(void)
{
	struct kmscon_vte_screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	struct kmscon_color fg, bg;

	vte = new_vte(&sink);

	/* default colors come from the legacy palette */
	input_str(vte, "A");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 0)->fg.r == 229);
	assert(cell_at(&screen, 0, 0)->bg.r == 0);

	/* palette colors, bold and underline */
	input_str(vte, "\033[31;1;4mB");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 1, 0)->ch == 'B');
	assert(cell_at(&screen, 1, 0)->fg.r == 205);
	assert(cell_at(&screen, 1, 0)->fg.g == 0);
	assert(cell_at(&screen, 1, 0)->attr.bold);
	assert(cell_at(&screen, 1, 0)->attr.underline);

	/* true color */
	input_str(vte, "\033[0;38;2;10;20;30mC");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 2, 0)->fg.r == 10);
	assert(cell_at(&screen, 2, 0)->fg.g == 20);
	assert(cell_at(&screen, 2, 0)->fg.b == 30);
	assert(!cell_at(&screen, 2, 0)->attr.bold);

	/* inverse swaps fore- and background */
	input_str(vte, "\033[0mD");
	assert(!kmscon_vte_draw(vte, &screen));
	fg = cell_at(&screen, 3, 0)->fg;
	bg = cell_at(&screen, 3, 0)->bg;
	input_str(vte, "\033[7mE");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 4, 0)->fg.r == bg.r && cell_at(&screen, 4, 0)->fg.g == bg.g);
	assert(cell_at(&screen, 4, 0)->bg.r == fg.r && cell_at(&screen, 4, 0)->bg.g == fg.g);

	/* blinking is reported to the renderers */
	input_str(vte, "\033[0;5mF");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 5, 0)->attr.blink);

	/* wide characters occupy two cells, the second one stays empty so the
	 * renderers can treat it as an overflow cell */
	input_str(vte, "\033[0m\r\n\344\275\240x");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 1)->ch == 0x4f60);
	assert(cell_at(&screen, 1, 1)->ch == 0);
	assert(cell_at(&screen, 2, 1)->ch == 'x');

	/* combining marks are folded into the base cell */
	input_str(vte, "\r\ne\314\201y");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 2)->ch == 'e');
	assert(cell_at(&screen, 1, 2)->ch == 'y');

	kmscon_vte_free(vte);
}

static void test_palette(void)
{
	struct kmscon_vte_screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	uint8_t custom[KMSCON_COLOR_NUM][3] = {0};

	vte = new_vte(&sink);

	assert(!kmscon_vte_set_palette(vte, "solarized", NULL));
	input_str(vte, "\033[34mA");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 0)->fg.r == 38);
	assert(cell_at(&screen, 0, 0)->fg.g == 139);
	assert(cell_at(&screen, 0, 0)->fg.b == 210);

	custom[KMSCON_COLOR_BLUE][0] = 1;
	custom[KMSCON_COLOR_BLUE][1] = 2;
	custom[KMSCON_COLOR_BLUE][2] = 3;
	assert(!kmscon_vte_set_palette(vte, "custom", custom));
	input_str(vte, "B");
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 1, 0)->fg.r == 1);
	assert(cell_at(&screen, 1, 0)->fg.g == 2);
	assert(cell_at(&screen, 1, 0)->fg.b == 3);

	kmscon_vte_free(vte);
}

static void test_scrollback(void)
{
	struct kmscon_vte_screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7");

	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 4)->ch == '7');

	kmscon_vte_sb_up(vte, 2);
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 4)->ch == '5');

	kmscon_vte_sb_down(vte, 1);
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 4)->ch == '6');

	/* a page up is clamped to the top of the scrollback */
	kmscon_vte_sb_page_up(vte, 1);
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 0)->ch == '1');
	assert(cell_at(&screen, 0, 4)->ch == '5');

	kmscon_vte_sb_reset(vte);
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 4)->ch == '7');

	kmscon_vte_free(vte);
}

static void test_resize(void)
{
	struct kmscon_vte_screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "abc");

	assert(!kmscon_vte_resize(vte, 40, 10, 8, 16));
	assert(kmscon_vte_get_cols(vte) == 40);
	assert(kmscon_vte_get_rows(vte) == 10);

	assert(!kmscon_vte_draw(vte, &screen));
	assert(screen.cols == 40 && screen.rows == 10);
	assert(cell_at(&screen, 0, 0)->ch == 'a');

	kmscon_vte_free(vte);
}

static void test_selection(void)
{
	struct kmscon_vte_screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	char *copy = NULL;
	int len;

	vte = new_vte(&sink);
	input_str(vte, "one two");

	kmscon_vte_selection_start(vte, 0, 0);
	kmscon_vte_selection_target(vte, 2, 0);

	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 0)->fg.r == 0);
	assert(cell_at(&screen, 0, 0)->bg.r == 229);
	assert(cell_at(&screen, 3, 0)->bg.r == 0);

	len = kmscon_vte_selection_copy(vte, &copy);
	assert(len == 3);
	assert(copy && !strcmp(copy, "one"));
	free(copy);
	copy = NULL;

	kmscon_vte_selection_reset(vte);
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 0)->bg.r == 0);
	assert(kmscon_vte_selection_copy(vte, &copy) == 0);
	assert(!copy);

	/* double click selects a whole word */
	kmscon_vte_selection_word(vte, 5, 0);
	len = kmscon_vte_selection_copy(vte, &copy);
	assert(len == 3);
	assert(copy && !strcmp(copy, "two"));
	free(copy);

	kmscon_vte_free(vte);
}

static bool press(struct kmscon_vte *vte, uint16_t keycode, uint32_t ascii, uint32_t unicode,
		  unsigned int mods)
{
	/* the input layer reports XKB keycodes, not raw evdev ones */
	return kmscon_vte_handle_keyboard(vte, keycode + INPUT_KEYCODE_OFFSET, ascii, unicode,
					  mods);
}

static void reset_sink(struct sink *sink)
{
	sink->len = 0;
	sink->buf[0] = 0;
}

static void test_keyboard(void)
{
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	assert(press(vte, KEY_A, 'a', 'a', 0));
	assert(!strcmp(sink.buf, "a"));

	/* plain keys must not turn into function key sequences */
	reset_sink(&sink);
	assert(press(vte, KEY_SPACE, ' ', ' ', 0));
	assert(!strcmp(sink.buf, " "));

	reset_sink(&sink);
	assert(press(vte, KEY_X, 'x', 'x', INPUT_ALT_MASK));
	assert(!strcmp(sink.buf, "\033x"));

	reset_sink(&sink);
	assert(press(vte, KEY_ENTER, 0, '\r', 0));
	assert(!strcmp(sink.buf, "\r"));

	reset_sink(&sink);
	assert(press(vte, KEY_TAB, 0, '\t', 0));
	assert(!strcmp(sink.buf, "\t"));

	reset_sink(&sink);
	assert(press(vte, KEY_F1, 0, INPUT_INVALID, 0));
	assert(!strcmp(sink.buf, "\033OP"));

	reset_sink(&sink);
	assert(press(vte, KEY_HOME, 0, INPUT_INVALID, 0));
	assert(!strcmp(sink.buf, "\033[H"));

	reset_sink(&sink);
	assert(press(vte, KEY_C, 'c', 'c', INPUT_CONTROL_MASK));
	assert(sink.len == 1 && sink.buf[0] == 0x03);

	reset_sink(&sink);
	assert(press(vte, KEY_UP, 0, INPUT_INVALID, 0));
	assert(!strcmp(sink.buf, "\033[A"));

	/* DECCKM switches the cursor keys to application mode */
	reset_sink(&sink);
	input_str(vte, "\033[?1h");
	assert(press(vte, KEY_UP, 0, INPUT_INVALID, 0));
	assert(!strcmp(sink.buf, "\033OA"));

	/* backspace honors the backspace-sends-delete setting */
	reset_sink(&sink);
	kmscon_vte_set_backspace_sends_delete(vte, true);
	assert(press(vte, KEY_BACKSPACE, 0, INPUT_INVALID, 0));
	assert(sink.len == 1 && sink.buf[0] == 0x7f);

	reset_sink(&sink);
	kmscon_vte_set_backspace_sends_delete(vte, false);
	assert(press(vte, KEY_BACKSPACE, 0, INPUT_INVALID, 0));
	assert(sink.len == 1 && sink.buf[0] == 0x08);

	kmscon_vte_free(vte);
}

static void test_mouse(void)
{
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	assert(!kmscon_vte_get_mouse_tracking(vte));

	/* nothing is reported while mouse tracking is disabled */
	kmscon_vte_handle_mouse(vte, 8, 16, 0, KMSCON_MOUSE_PRESSED, 0);
	assert(!sink.len);

	/* normal tracking with SGR encoding */
	input_str(vte, "\033[?1000h\033[?1006h");
	assert(kmscon_vte_get_mouse_tracking(vte));
	assert(sink.tracking);

	kmscon_vte_handle_mouse(vte, 8, 16, 0, KMSCON_MOUSE_PRESSED, 0);
	assert(!strcmp(sink.buf, "\033[<0;2;2M"));

	reset_sink(&sink);
	kmscon_vte_handle_mouse(vte, 8, 16, 0, KMSCON_MOUSE_RELEASED, 0);
	assert(!strcmp(sink.buf, "\033[<0;2;2m"));

	input_str(vte, "\033[?1000l");
	assert(!kmscon_vte_get_mouse_tracking(vte));
	assert(!sink.tracking);

	kmscon_vte_free(vte);
}

static void test_paste(void)
{
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	kmscon_vte_paste(vte, "abc", 3);
	assert(!strcmp(sink.buf, "abc"));

	reset_sink(&sink);
	input_str(vte, "\033[?2004h");
	kmscon_vte_paste(vte, "abc", 3);
	assert(!strcmp(sink.buf, "\033[200~abc\033[201~"));

	kmscon_vte_free(vte);
}

static void test_effects(void)
{
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	input_str(vte, "\a");
	assert(sink.bells == 1);

	/* kmscon specific OSC commands, terminated by BEL and by ST */
	input_str(vte, "\033]setBackground\a");
	assert(sink.osc_calls == 1);
	assert(!strcmp(sink.osc, "setBackground"));

	input_str(vte, "\033]setForeground\033\\");
	assert(sink.osc_calls == 2);
	assert(!strcmp(sink.osc, "setForeground"));

	/* device status reports are answered on the pty */
	reset_sink(&sink);
	input_str(vte, "\033[6n");
	assert(!strcmp(sink.buf, "\033[1;1R"));

	kmscon_vte_free(vte);
}

static void test_reset(void)
{
	struct kmscon_vte_screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "\033[?25labc");

	kmscon_vte_hard_reset(vte);
	assert(!kmscon_vte_draw(vte, &screen));
	assert(cell_at(&screen, 0, 0)->ch == 0);
	assert(screen.cursor_visible);

	kmscon_vte_free(vte);
}

int main(void)
{
	test_text_and_cursor();
	test_attributes();
	test_palette();
	test_scrollback();
	test_resize();
	test_selection();
	test_keyboard();
	test_mouse();
	test_paste();
	test_effects();
	test_reset();

	printf("vte tests passed\n");
	return 0;
}
