/*
 * Tests for the vte layer, that is the libghostty-vt integration.
 */

#include <assert.h>
#include <errno.h>
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
	unsigned int copy_calls;
	char copy[256];
	size_t copy_len;
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

static void sink_copy(const char *u8, size_t len, void *data)
{
	struct sink *sink = data;

	sink->copy_calls++;
	assert(len < sizeof(sink->copy));
	if (len)
		memcpy(sink->copy, u8, len);
	sink->copy[len] = 0;
	sink->copy_len = len;
}

static void sink_mouse_mode(bool tracking, void *data)
{
	struct sink *sink = data;

	sink->tracking = tracking;
}

/* one renderer watching the terminal, reset for every new vte */
static struct kmscon_vte_watch watch;

static struct kmscon_vte *new_vte(struct sink *sink)
{
	struct kmscon_vte *vte = NULL;
	int ret;

	memset(sink, 0, sizeof(*sink));
	memset(&watch, 0, sizeof(watch));
	ret = kmscon_vte_new(&vte, 20, 5, 100, sink);
	assert(!ret);
	assert(vte);

	kmscon_vte_set_write_cb(vte, sink_write);
	kmscon_vte_set_bell_cb(vte, sink_bell);
	kmscon_vte_set_osc_cb(vte, sink_osc);
	kmscon_vte_set_mouse_mode_cb(vte, sink_mouse_mode);
	kmscon_vte_set_copy_cb(vte, sink_copy);
	kmscon_vte_resize(vte, 20, 5, 8, 16);
	return vte;
}

static void input_str(struct kmscon_vte *vte, const char *str)
{
	kmscon_vte_input(vte, str, strlen(str));
}

/*
 * The renderers pull a frame row by row and are told which rows changed. The
 * tests want to look at the whole screen at once, so flatten it here, the way
 * a renderer without a cache of its own would.
 */
#define TEST_COLS 64
#define TEST_ROWS 32

struct screen {
	struct kmscon_cell cells[TEST_COLS * TEST_ROWS];
	unsigned int cols;
	unsigned int rows;
	struct kmscon_screen_attr attr;

	/* which rows the vte reported as changed since the previous draw */
	bool dirty[TEST_ROWS];
	unsigned int dirty_rows;
};

/* Returns 1 if the vte had anything new to show, 0 if the frame was clean */
static int draw(struct kmscon_vte *vte, struct screen *screen)
{
	struct kmscon_vte_frame frame;
	unsigned int y;
	bool dirty;
	int changed;

	memset(screen, 0, sizeof(*screen));

	changed = kmscon_vte_frame_begin(vte, &watch, &frame);
	assert(changed >= 0);
	assert(frame.cols <= TEST_COLS && frame.rows <= TEST_ROWS);

	screen->cols = frame.cols;
	screen->rows = frame.rows;
	screen->attr = frame.attr;

	while (kmscon_vte_frame_row(vte, &y, &dirty)) {
		kmscon_vte_frame_cells(vte, &screen->cells[y * frame.cols], frame.cols);
		screen->dirty[y] = dirty;
		screen->dirty_rows += dirty;
	}

	kmscon_vte_frame_end(vte, &watch);
	return changed;
}

static const struct kmscon_cell *cell_at(struct screen *screen, unsigned int x, unsigned int y)
{
	return &screen->cells[x + y * screen->cols];
}

/*
 * The cursor is folded into the cell it sits on rather than drawn over it, so
 * a block cursor shows up as a cell wearing the colors of the screen the wrong
 * way round.
 */
static bool block_cursor_at(struct screen *screen, unsigned int x, unsigned int y)
{
	const struct kmscon_cell *cell = cell_at(screen, x, y);

	return !memcmp(&cell->fg, &screen->attr.bg, sizeof(cell->fg)) &&
	       !memcmp(&cell->bg, &screen->attr.fg, sizeof(cell->bg));
}

static void test_text_and_cursor(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "hello\r\nworld");

	draw(vte, &screen);
	assert(screen.cols == 20 && screen.rows == 5);
	assert(cell_at(&screen, 0, 0)->ch == 'h');
	assert(cell_at(&screen, 4, 0)->ch == 'o');
	assert(cell_at(&screen, 0, 1)->ch == 'w');
	assert(cell_at(&screen, 5, 0)->ch == 0);
	/* the cursor sits behind the last character it wrote */
	assert(block_cursor_at(&screen, 5, 1));

	/* DECTCEM hides the cursor */
	input_str(vte, "\033[?25l");
	draw(vte, &screen);
	assert(!block_cursor_at(&screen, 5, 1));

	kmscon_vte_free(vte);
}

/*
 * The heart of the frame model: a renderer is told exactly which rows moved
 * since it last drew, and nothing more.
 */
static void test_dirty_rows(void)
{
	struct kmscon_vte_frame frame;
	struct kmscon_vte_watch late;
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	/* a renderer that has never drawn is handed the whole screen */
	assert(draw(vte, &screen) == 1);
	assert(screen.dirty_rows == screen.rows);

	/* a terminal that stands still costs a renderer nothing */
	assert(draw(vte, &screen) == 0);
	assert(screen.dirty_rows == 0);

	/* writing into one line only changes that line */
	input_str(vte, "hello");
	assert(draw(vte, &screen) == 1);
	assert(screen.dirty[0]);
	assert(screen.dirty_rows == 1);

	/* moving to the next one changes the row the cursor left as well */
	input_str(vte, "\r\nworld");
	assert(draw(vte, &screen) == 1);
	assert(screen.dirty[0] && screen.dirty[1]);
	assert(screen.dirty_rows == 2);

	/* scrolling moves every row */
	input_str(vte, "\r\n\r\n\r\n\r\n\r\n\r\n");
	assert(draw(vte, &screen) == 1);
	assert(screen.dirty_rows == screen.rows);

	/*
	 * A second display showing the same terminal keeps its own place in
	 * the history, so falling behind does not cost it the frame.
	 */
	assert(draw(vte, &screen) == 0);
	memset(&late, 0, sizeof(late));
	assert(kmscon_vte_frame_begin(vte, &late, &frame) == 1);

	/* selecting inverts cells the terminal itself never touched */
	kmscon_vte_selection_start(vte, 0, 0);
	kmscon_vte_selection_target(vte, 4, 0);
	assert(draw(vte, &screen) == 1);
	assert(screen.dirty_rows == screen.rows);

	kmscon_vte_free(vte);
}

/* A blinking cell is blank for half of its period, and only the rows that
 * hold one are repainted when the phase flips. */
static void test_blink(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "\033[5mblink\033[0m");

	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->attr.blink);
	assert(cell_at(&screen, 0, 0)->ch == 'b');

	kmscon_vte_blink_tick(vte);
	assert(draw(vte, &screen) == 1);
	assert(cell_at(&screen, 0, 0)->ch == ' ');
	assert(screen.dirty[0]);
	/* the empty rows below hold nothing that blinks */
	assert(screen.dirty_rows == 1);

	kmscon_vte_blink_tick(vte);
	assert(draw(vte, &screen) == 1);
	assert(cell_at(&screen, 0, 0)->ch == 'b');

	kmscon_vte_free(vte);
}

static void test_attributes(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	struct kmscon_color fg, bg;

	vte = new_vte(&sink);

	/* default colors come from the legacy palette */
	input_str(vte, "A");
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->fg.r == 229);
	assert(cell_at(&screen, 0, 0)->bg.r == 0);

	/* palette colors, bold and underline */
	input_str(vte, "\033[31;1;4mB");
	draw(vte, &screen);
	assert(cell_at(&screen, 1, 0)->ch == 'B');
	assert(cell_at(&screen, 1, 0)->fg.r == 205);
	assert(cell_at(&screen, 1, 0)->fg.g == 0);
	assert(cell_at(&screen, 1, 0)->attr.bold);
	assert(cell_at(&screen, 1, 0)->attr.underline);

	/* true color */
	input_str(vte, "\033[0;38;2;10;20;30mC");
	draw(vte, &screen);
	assert(cell_at(&screen, 2, 0)->fg.r == 10);
	assert(cell_at(&screen, 2, 0)->fg.g == 20);
	assert(cell_at(&screen, 2, 0)->fg.b == 30);
	assert(!cell_at(&screen, 2, 0)->attr.bold);

	/* inverse swaps fore- and background */
	input_str(vte, "\033[0mD");
	draw(vte, &screen);
	fg = cell_at(&screen, 3, 0)->fg;
	bg = cell_at(&screen, 3, 0)->bg;
	input_str(vte, "\033[7mE");
	draw(vte, &screen);
	assert(cell_at(&screen, 4, 0)->fg.r == bg.r && cell_at(&screen, 4, 0)->fg.g == bg.g);
	assert(cell_at(&screen, 4, 0)->bg.r == fg.r && cell_at(&screen, 4, 0)->bg.g == fg.g);

	/* blinking is reported to the renderers */
	input_str(vte, "\033[0;5mF");
	draw(vte, &screen);
	assert(cell_at(&screen, 5, 0)->attr.blink);

	/* wide characters occupy two cells, the second one stays empty so the
	 * renderers can treat it as an overflow cell */
	input_str(vte, "\033[0m\r\n\344\275\240x");
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 1)->ch == 0x4f60);
	assert(cell_at(&screen, 1, 1)->ch == 0);
	assert(cell_at(&screen, 2, 1)->ch == 'x');

	/* combining marks are folded into the base cell */
	input_str(vte, "\r\ne\314\201y");
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 2)->ch == 'e');
	assert(cell_at(&screen, 1, 2)->ch == 'y');

	kmscon_vte_free(vte);
}

static void test_palette(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	uint8_t custom[KMSCON_COLOR_NUM][3] = {0};

	vte = new_vte(&sink);

	assert(!kmscon_vte_set_palette(vte, "solarized", NULL));
	input_str(vte, "\033[34mA");
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->fg.r == 38);
	assert(cell_at(&screen, 0, 0)->fg.g == 139);
	assert(cell_at(&screen, 0, 0)->fg.b == 210);

	custom[KMSCON_COLOR_BLUE][0] = 1;
	custom[KMSCON_COLOR_BLUE][1] = 2;
	custom[KMSCON_COLOR_BLUE][2] = 3;
	assert(!kmscon_vte_set_palette(vte, "custom", custom));
	input_str(vte, "B");
	draw(vte, &screen);
	assert(cell_at(&screen, 1, 0)->fg.r == 1);
	assert(cell_at(&screen, 1, 0)->fg.g == 2);
	assert(cell_at(&screen, 1, 0)->fg.b == 3);

	kmscon_vte_free(vte);
}

static void test_scrollback(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7");

	draw(vte, &screen);
	assert(cell_at(&screen, 0, 4)->ch == '7');

	kmscon_vte_sb_up(vte, 2);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 4)->ch == '5');

	kmscon_vte_sb_down(vte, 1);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 4)->ch == '6');

	/* a page up is clamped to the top of the scrollback */
	kmscon_vte_sb_page_up(vte, 1);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->ch == '1');
	assert(cell_at(&screen, 0, 4)->ch == '5');

	kmscon_vte_sb_reset(vte);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 4)->ch == '7');

	kmscon_vte_free(vte);
}

static void test_resize(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "abc");

	assert(!kmscon_vte_resize(vte, 40, 10, 8, 16));
	assert(kmscon_vte_get_cols(vte) == 40);
	assert(kmscon_vte_get_rows(vte) == 10);

	draw(vte, &screen);
	assert(screen.cols == 40 && screen.rows == 10);
	assert(cell_at(&screen, 0, 0)->ch == 'a');

	kmscon_vte_free(vte);
}

static void test_selection(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	char *copy = NULL;
	int len;

	vte = new_vte(&sink);
	input_str(vte, "one two");

	kmscon_vte_selection_start(vte, 0, 0);
	kmscon_vte_selection_target(vte, 2, 0);

	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->fg.r == 0);
	assert(cell_at(&screen, 0, 0)->bg.r == 229);
	assert(cell_at(&screen, 3, 0)->bg.r == 0);

	len = kmscon_vte_selection_copy(vte, &copy);
	assert(len == 3);
	assert(copy && !strcmp(copy, "one"));
	free(copy);
	copy = NULL;

	kmscon_vte_selection_reset(vte);
	draw(vte, &screen);
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

/* Feed a prompt, the command the user typed and its output, the way a shell
 * with kmscon's shell integration marks them up. */
static void shell_command(struct kmscon_vte *vte, const char *cmd, const char *output)
{
	input_str(vte, "\033]133;A\007$ \033]133;B\007");
	input_str(vte, cmd);
	input_str(vte, "\033]133;C\007\r\n");
	input_str(vte, output);
	input_str(vte, "\r\n\033]133;D;0\007");
}

static void test_shell_integration(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	char *copy = NULL;
	int len;

	vte = new_vte(&sink);

	/* five prompts, each with one line of output, so that the screen of
	 * five rows leaves earlier prompts in the scrollback */
	shell_command(vte, "one", "first");
	shell_command(vte, "two", "second");
	shell_command(vte, "three", "third");
	shell_command(vte, "four", "fourth");
	shell_command(vte, "five", "fifth");
	input_str(vte, "\033]133;A\007$ \033]133;B\007");

	/* jumping up puts the previous prompt at the top of the viewport */
	assert(!kmscon_vte_jump_to_prompt(vte, -1));
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->ch == '$');
	assert(cell_at(&screen, 2, 0)->ch == 't');
	assert(cell_at(&screen, 3, 0)->ch == 'h');

	/* and again for the prompt above that one, output lines are skipped */
	assert(!kmscon_vte_jump_to_prompt(vte, -1));
	draw(vte, &screen);
	assert(cell_at(&screen, 2, 0)->ch == 't');
	assert(cell_at(&screen, 3, 0)->ch == 'w');

	/* jumping back down returns to the later prompt */
	assert(!kmscon_vte_jump_to_prompt(vte, 1));
	draw(vte, &screen);
	assert(cell_at(&screen, 3, 0)->ch == 'h');

	/* running out of prompts is reported and the viewport does not move */
	assert(kmscon_vte_jump_to_prompt(vte, -100) == -ENOENT);
	draw(vte, &screen);
	assert(cell_at(&screen, 3, 0)->ch == 'h');

	assert(kmscon_vte_jump_to_prompt(vte, 0) == -EINVAL);

	/* selecting the output of a command picks up that command's output
	 * only, not the prompt or the command line */
	kmscon_vte_sb_reset(vte);
	kmscon_vte_selection_output(vte, 0, 3);
	len = kmscon_vte_selection_copy(vte, &copy);
	assert(len > 0);
	assert(!strcmp(copy, "fifth"));
	free(copy);

	kmscon_vte_free(vte);
}

/*
 * The cursor is folded into the cell it sits on, so every shape, color and
 * blink phase is checked on that cell rather than on a flag.
 */
static void test_cursor(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;
	uint8_t rgb[3] = {0xff, 0x00, 0x88};

	vte = new_vte(&sink);

	/* a block cursor that does not blink is the built-in default */
	draw(vte, &screen);
	assert(block_cursor_at(&screen, 0, 0));
	kmscon_vte_cursor_blink_tick(vte);
	draw(vte, &screen);
	assert(block_cursor_at(&screen, 0, 0));
	kmscon_vte_cursor_blink_reset(vte);

	assert(!kmscon_vte_set_cursor_shape(vte, "bar"));
	kmscon_vte_set_cursor_blink(vte, true);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->attr.cursor_bar);

	/* half of a blinking cursor's period has no cursor in it at all */
	kmscon_vte_cursor_blink_tick(vte);
	draw(vte, &screen);
	assert(!cell_at(&screen, 0, 0)->attr.cursor_bar);
	kmscon_vte_cursor_blink_reset(vte);

	assert(!kmscon_vte_set_cursor_shape(vte, "underline"));
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->attr.underline);
	assert(!cell_at(&screen, 0, 0)->attr.cursor_bar);

	assert(!kmscon_vte_set_cursor_shape(vte, "hollow"));
	draw(vte, &screen);
	assert(block_cursor_at(&screen, 0, 0));

	assert(kmscon_vte_set_cursor_shape(vte, "wobbly"));

	/* DECSCUSR wins over the configured default */
	input_str(vte, "\033[5 q");
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->attr.cursor_bar);

	input_str(vte, "\033[2 q");
	draw(vte, &screen);
	assert(block_cursor_at(&screen, 0, 0));
	/* DECSCUSR 2 is the steady variant, so the blink phase changes nothing */
	kmscon_vte_cursor_blink_tick(vte);
	draw(vte, &screen);
	assert(block_cursor_at(&screen, 0, 0));
	kmscon_vte_cursor_blink_reset(vte);

	/* and a reset goes back to what we configured */
	input_str(vte, "\033[0 q");
	draw(vte, &screen);
	assert(block_cursor_at(&screen, 0, 0));

	assert(!kmscon_vte_set_cursor_shape(vte, "bar"));
	kmscon_vte_set_cursor_color(vte, rgb);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->fg.r == 0xff);
	assert(cell_at(&screen, 0, 0)->fg.g == 0x00);
	assert(cell_at(&screen, 0, 0)->fg.b == 0x88);

	/* a block cursor is filled with that color instead */
	assert(!kmscon_vte_set_cursor_shape(vte, "block"));
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->bg.r == 0xff);
	assert(cell_at(&screen, 0, 0)->bg.b == 0x88);

	/* clearing it goes back to inverting the cell below the cursor */
	kmscon_vte_set_cursor_color(vte, NULL);
	draw(vte, &screen);
	assert(block_cursor_at(&screen, 0, 0));

	/* OSC 12 lets the application pick a color as well */
	input_str(vte, "\033]12;#00ff00\a");
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->bg.g == 0xff);

	/* that one outlives a change of our default, it is the application's */
	kmscon_vte_set_cursor_color(vte, NULL);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->bg.g == 0xff);

	kmscon_vte_free(vte);
}

static void test_clipboard_write(void)
{
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	/* OSC 52 to the standard clipboard, "hello" in base64 */
	input_str(vte, "\033]52;c;aGVsbG8=\a");
	assert(sink.copy_calls == 1);
	assert(sink.copy_len == 5);
	assert(!strcmp(sink.copy, "hello"));

	/* the primary selection ends up in the same buffer */
	input_str(vte, "\033]52;p;d29ybGQ=\033\\");
	assert(sink.copy_calls == 2);
	assert(!strcmp(sink.copy, "world"));

	/* an empty payload clears the buffer */
	input_str(vte, "\033]52;c;\a");
	assert(sink.copy_calls == 3);
	assert(!sink.copy_len);

	/* a read request is never forwarded to us */
	input_str(vte, "\033]52;c;?\a");
	assert(sink.copy_calls == 3);

	/* invalid base64 is rejected before it reaches the callback */
	input_str(vte, "\033]52;c;!!!!\a");
	assert(sink.copy_calls == 3);

	/* without a copy callback the terminal ignores clipboard writes */
	kmscon_vte_set_copy_cb(vte, NULL);
	input_str(vte, "\033]52;c;aGVsbG8=\a");
	assert(sink.copy_calls == 3);

	kmscon_vte_free(vte);
}

static void test_focus(void)
{
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	/* without focus events enabled nothing is reported */
	kmscon_vte_set_focus(vte, true);
	kmscon_vte_set_focus(vte, false);
	assert(!sink.len);

	input_str(vte, "\033[?1004h");
	kmscon_vte_set_focus(vte, true);
	assert(!strcmp(sink.buf, "\033[I"));

	/* the same state twice is not reported again */
	reset_sink(&sink);
	kmscon_vte_set_focus(vte, true);
	assert(!sink.len);

	kmscon_vte_set_focus(vte, false);
	assert(!strcmp(sink.buf, "\033[O"));

	reset_sink(&sink);
	input_str(vte, "\033[?1004l");
	kmscon_vte_set_focus(vte, true);
	assert(!sink.len);

	kmscon_vte_free(vte);
}

static void test_color_scheme(void)
{
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	/* the legacy palette has a black background, so we are dark */
	input_str(vte, "\033[?996n");
	assert(!strcmp(sink.buf, "\033[?997;1n"));

	reset_sink(&sink);
	assert(!kmscon_vte_set_palette(vte, "solarized-white", NULL));
	input_str(vte, "\033[?996n");
	assert(!strcmp(sink.buf, "\033[?997;2n"));

	/* with mode 2031 set a palette change is reported on its own */
	reset_sink(&sink);
	input_str(vte, "\033[?2031h");
	assert(!kmscon_vte_set_palette(vte, "legacy", NULL));
	assert(!strcmp(sink.buf, "\033[?997;1n"));

	kmscon_vte_free(vte);
}

static void test_min_contrast(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);

	/* black on black is unreadable, but is left alone by default */
	input_str(vte, "\033[30;40mA");
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->fg.r == 0);
	assert(cell_at(&screen, 0, 0)->bg.r == 0);

	kmscon_vte_set_min_contrast(vte, 4.5);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->fg.r == 0xff);
	assert(cell_at(&screen, 0, 0)->bg.r == 0);

	/* white on black already has the highest contrast there is */
	input_str(vte, "\033[37;40mB");
	draw(vte, &screen);
	assert(cell_at(&screen, 1, 0)->fg.r == 229);

	/* on a light background the text is pushed to black instead */
	input_str(vte, "\033[30;47mC");
	draw(vte, &screen);
	assert(cell_at(&screen, 2, 0)->fg.r == 0);
	assert(cell_at(&screen, 2, 0)->bg.r == 229);

	kmscon_vte_free(vte);
}

static void test_parse_color(void)
{
	uint8_t color[3];

	assert(!kmscon_vte_parse_color("#1e1e2e", color));
	assert(color[0] == 0x1e && color[1] == 0x1e && color[2] == 0x2e);

	assert(!kmscon_vte_parse_color("cornflowerblue", color));
	assert(color[0] == 100 && color[1] == 149 && color[2] == 237);

	assert(!kmscon_vte_parse_color("rgb:12/34/56", color));
	assert(color[0] == 0x12 && color[1] == 0x34 && color[2] == 0x56);

	assert(kmscon_vte_parse_color("not-a-color", color));
	assert(kmscon_vte_parse_color("", color));

	assert(kmscon_vte_parse_color(NULL, color));
	assert(kmscon_vte_parse_color("#000000", NULL));
}

static void test_scrollbar(void)
{
	struct kmscon_vte_scrollbar sb;
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7");

	kmscon_vte_get_scrollbar(vte, &sb);
	assert(sb.len == 5);
	assert(sb.total == 7);
	assert(sb.offset == 2);

	/* the last column is only taken over once it is enabled */
	kmscon_vte_sb_up(vte, 2);
	draw(vte, &screen);
	assert(cell_at(&screen, 19, 0)->ch == 0);

	kmscon_vte_set_scrollbar(vte, true);
	draw(vte, &screen);
	assert(cell_at(&screen, 19, 0)->ch == 0x2588);
	assert(cell_at(&screen, 19, 4)->ch == 0x2502);

	kmscon_vte_get_scrollbar(vte, &sb);
	assert(sb.offset == 0);

	/* back at the bottom the column belongs to the terminal again */
	kmscon_vte_sb_reset(vte);
	draw(vte, &screen);
	assert(cell_at(&screen, 19, 0)->ch == 0);

	kmscon_vte_free(vte);
}

static void test_reset(void)
{
	struct screen screen;
	struct kmscon_vte *vte;
	struct sink sink;

	vte = new_vte(&sink);
	input_str(vte, "\033[?25labc");

	kmscon_vte_hard_reset(vte);
	draw(vte, &screen);
	assert(cell_at(&screen, 0, 0)->ch == 0);
	assert(block_cursor_at(&screen, 0, 0));

	kmscon_vte_free(vte);
}

int main(void)
{
	test_text_and_cursor();
	test_dirty_rows();
	test_blink();
	test_attributes();
	test_palette();
	test_scrollback();
	test_resize();
	test_selection();
	test_keyboard();
	test_mouse();
	test_paste();
	test_effects();
	test_shell_integration();
	test_cursor();
	test_clipboard_write();
	test_focus();
	test_color_scheme();
	test_min_contrast();
	test_parse_color();
	test_scrollbar();
	test_reset();

	printf("vte tests passed\n");
	return 0;
}
