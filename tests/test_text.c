/*
 * Lightweight test for kmscon_text_set / kmscon_text_unset.
 * We avoid linking the whole tree by stubbing external deps.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../src/render/text.c" /* pull in kmscon_text_set without changing meson */

/* --- Stubs for external functions used by kmscon_text_set --- */
static struct kmscon_vte_screen fake_screen;
static bool fake_screen_valid;

int kmscon_vte_draw(struct kmscon_vte *vte, struct kmscon_vte_screen *out)
{
	if (!fake_screen_valid)
		return -EINVAL;

	*out = fake_screen;
	return 0;
}
void kmscon_font_ref(struct kmscon_font *font) {}
void kmscon_font_unref(struct kmscon_font *font) {}
void display_ref(struct display *disp) {}
void display_unref(struct display *disp) {}

static int dummy_set_calls;
static int dummy_unset_calls;
static int dummy_set(struct kmscon_text *txt)
{
	dummy_set_calls++;
	return 0;
}
static void dummy_unset(struct kmscon_text *txt)
{
	dummy_unset_calls++;
}

static struct kmscon_cursor drawn_cursor;
static int dummy_draw(struct kmscon_text *txt, const struct kmscon_cell *cells,
		      struct kmscon_cursor *cursor)
{
	drawn_cursor = *cursor;
	return 0;
}

static struct kmscon_text_ops dummy_ops = {
	.name = "dummytest",
	.owner = NULL,
	.init = NULL,
	.destroy = NULL,
	.set = dummy_set,
	.unset = dummy_unset,
	.draw = dummy_draw,
};

/* The renderers draw a single glyph per cell, so the cursor has to be folded
 * into the cell below it without losing the character that is there. */
static void test_cursor_cell(void)
{
	struct kmscon_cell cells[2];
	struct kmscon_text txt;
	struct kmscon_vte *fake_vte = (struct kmscon_vte *)0x1;

	memset(&txt, 0, sizeof(txt));
	txt.ops = &dummy_ops;
	txt.cols = 2;
	txt.rows = 1;

	memset(cells, 0, sizeof(cells));
	cells[0].ch = 'A';
	cells[0].fg = (struct kmscon_color){0x10, 0x20, 0x30};
	cells[0].bg = (struct kmscon_color){0x40, 0x50, 0x60};

	memset(&fake_screen, 0, sizeof(fake_screen));
	fake_screen.cells = cells;
	fake_screen.cols = 2;
	fake_screen.rows = 1;
	fake_screen.cursor_visible = true;
	fake_screen_valid = true;

	/* a block cursor inverts the cell and keeps the character */
	fake_screen.cursor_shape = KMSCON_CURSOR_BLOCK;
	assert(!kmscon_text_draw(&txt, fake_vte, false));
	assert(drawn_cursor.visible);
	assert(drawn_cursor.cell.ch == 'A');
	assert(drawn_cursor.cell.fg.r == 0x40);
	assert(drawn_cursor.cell.bg.r == 0x10);
	assert(!drawn_cursor.cell.attr.cursor_bar);

	/* an underline cursor keeps the character and underlines it */
	fake_screen.cursor_shape = KMSCON_CURSOR_UNDERLINE;
	assert(!kmscon_text_draw(&txt, fake_vte, false));
	assert(drawn_cursor.cell.ch == 'A');
	assert(drawn_cursor.cell.attr.underline);
	assert(drawn_cursor.cell.fg.r == 0x10);

	/* a bar cursor must not hide the character either, the bar is drawn
	 * into the glyph by the font layer */
	fake_screen.cursor_shape = KMSCON_CURSOR_BAR;
	assert(!kmscon_text_draw(&txt, fake_vte, false));
	assert(drawn_cursor.cell.ch == 'A');
	assert(drawn_cursor.cell.attr.cursor_bar);
	assert(!drawn_cursor.cell.attr.underline);

	/* the same for an empty cell, where there is nothing to keep */
	cells[0].ch = 0;
	assert(!kmscon_text_draw(&txt, fake_vte, false));
	assert(drawn_cursor.cell.ch == 0);
	assert(drawn_cursor.cell.attr.cursor_bar);
	cells[0].ch = 'A';

	/* a configured cursor color colors the bar */
	fake_screen.cursor_has_color = true;
	fake_screen.cursor_color = (struct kmscon_color){0xaa, 0xbb, 0xcc};
	assert(!kmscon_text_draw(&txt, fake_vte, false));
	assert(drawn_cursor.cell.fg.r == 0xaa);

	/* and fills a block cursor */
	fake_screen.cursor_shape = KMSCON_CURSOR_BLOCK;
	assert(!kmscon_text_draw(&txt, fake_vte, false));
	assert(drawn_cursor.cell.bg.r == 0xaa);
	assert(drawn_cursor.cell.fg.r == 0x40);

	fake_screen_valid = false;
}

/* The bar is painted into the left edge of the glyph buffer */
static void test_glyph_vbar(void)
{
	struct {
		struct video_buffer buf;
		uint8_t data[8 * 4];
	} glyph;

	memset(&glyph, 0, sizeof(glyph));
	glyph.buf.width = 8;
	glyph.buf.height = 4;

	kmscon_glyph_draw_vbar(&glyph.buf, 8);

	assert(glyph.buf.data[0] == 0xff);
	assert(glyph.buf.data[1] == 0);
	assert(glyph.buf.data[3 * 8] == 0xff);
	assert(glyph.buf.data[3 * 8 + 7] == 0);
}

int main(void)
{
	struct kmscon_text txt;
	struct kmscon_font fake_font;
	struct display *fake_disp = (struct display *)0x1;
	int ret;

	memset(&txt, 0, sizeof(txt));
	memset(&fake_font, 0, sizeof(fake_font));
	txt.ops = &dummy_ops;

	/* set calls backend set */
	ret = kmscon_text_set(&txt, &fake_font, fake_disp);
	assert(ret == 0);
	assert(dummy_set_calls == 1);
	assert(txt.font == &fake_font);
	assert(txt.disp == fake_disp);

	/* unset calls backend unset and clears pointers */
	kmscon_text_unset(&txt);
	assert(dummy_unset_calls == 1);
	assert(txt.font == NULL);
	assert(txt.disp == NULL);

	/* NULL font must return -EINVAL */
	ret = kmscon_text_set(&txt, NULL, fake_disp);
	assert(ret == -EINVAL);
	assert(dummy_set_calls == 1); /* not called again */

	test_cursor_cell();
	test_glyph_vbar();

	return 0;
}
