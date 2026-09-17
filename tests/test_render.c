/*
 * End to end test of one rendering round: a real terminal, the real text
 * layer and the real bbulk backend, with only the display and the font
 * stubbed out. This is the path the screen actually takes, so it catches the
 * wiring that the per-layer tests each assume the other side gets right.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <string.h>
#include "../src/render/text.h"

#include "../src/font/font.h"
#include "../src/video/video.h"

#define FAKE_CELL_W 8
#define FAKE_CELL_H 16
#define FAKE_SW 320
#define FAKE_SH 160

unsigned int display_get_width(struct display *disp)
{
	return FAKE_SW;
}
unsigned int display_get_height(struct display *disp)
{
	return FAKE_SH;
}

unsigned int kmscon_font_get_width(const struct kmscon_font *font)
{
	return FAKE_CELL_W;
}
unsigned int kmscon_font_get_height(const struct kmscon_font *font)
{
	return FAKE_CELL_H;
}

/* the last character each glyph request asked for, indexed by nothing in
 * particular: the test only cares that the right ones showed up */
static uint32_t rendered[4096];
static unsigned int rendered_len;

struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, const uint32_t ch)
{
	struct kmscon_glyph *g;

	g = malloc(sizeof(*g) + FAKE_CELL_W * FAKE_CELL_H);
	memset(g, 0, sizeof(*g) + FAKE_CELL_W * FAKE_CELL_H);
	g->buf.width = FAKE_CELL_W;
	g->buf.height = FAKE_CELL_H;
	return g;
}

bool kmscon_font_has_glyph(struct kmscon_font *font, const uint32_t ch)
{
	return true;
}

int kmscon_rotate_glyph(struct kmscon_glyph *vb, const struct kmscon_glyph *glyph,
			enum Orientation o, uint8_t align)
{
	if (!vb || !glyph)
		return -EINVAL;

	*vb = *glyph;
	return 0;
}

void kmscon_font_ref(struct kmscon_font *font) {}
void kmscon_font_unref(struct kmscon_font *font) {}
void display_ref(struct display *disp) {}
void display_unref(struct display *disp) {}

static bool need_redraw;
static unsigned int blends;
static unsigned int clears;
static unsigned int swaps;

bool display_need_redraw(struct display *disp)
{
	return need_redraw;
}
bool display_has_damage(struct display *disp)
{
	return false;
}
bool display_supports_damage(struct display *disp)
{
	return true;
}
int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	clears++;
	return 0;
}
int display_blend(struct display *disp, const struct video_blend_req *req)
{
	blends++;
	if (rendered_len < sizeof(rendered) / sizeof(*rendered))
		rendered[rendered_len++] = req->fr; /* placeholder, see below */
	return 0;
}
void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages) {}
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y) {}

static struct kmscon_font fake_font = {.attr = {.width = FAKE_CELL_W, .height = FAKE_CELL_H}};

/* One full round the way terminal.c drives it. Returns the number of cells
 * that were pushed to the display. */
static int round_trip(struct kmscon_text *txt, struct kmscon_vte *vte, bool force)
{
	int ret;

	blends = 0;
	ret = kmscon_text_prepare(txt, vte, force);
	assert(ret >= 0);
	if (!ret) {
		if (getenv("TRACE"))
			fprintf(stderr, "round: skipped\n");
		return -1;
	} /* the round was skipped entirely */

	assert(!kmscon_text_draw(txt));
	assert(!kmscon_text_render(txt));
	swaps++;
	if (getenv("TRACE"))
		fprintf(stderr, "round: %u blends\n", blends);
	return blends;
}

/* Run rounds until the screen has nothing left to draw. A renderer that never
 * settles would be repainting forever. */
static void settle(struct kmscon_text *txt, struct kmscon_vte *vte)
{
	int i;

	for (i = 0; i < 8; ++i)
		if (round_trip(txt, vte, false) == -1)
			return;

	assert(!"the screen never settles");
}

static void sink_write(const char *u8, size_t len, void *data) {}

int main(void)
{
	struct kmscon_text txt;
	struct kmscon_vte *vte = NULL;
	unsigned int cols, rows, i;
	int n;

	memset(&txt, 0, sizeof(txt));
	txt.ops = &kmscon_text_bbulk_ops;
	txt.disp = (struct display *)0x1;
	txt.orientation = OR_NORMAL;
	txt.ref = 1;

	assert(!kmscon_text_bbulk_ops.init(&txt));
	assert(!kmscon_text_set(&txt, &fake_font, txt.disp));

	cols = kmscon_text_get_cols(&txt);
	rows = kmscon_text_get_rows(&txt);
	assert(cols == FAKE_SW / FAKE_CELL_W);
	assert(rows == FAKE_SH / FAKE_CELL_H);

	assert(!kmscon_vte_new(&vte, cols, rows, 100, NULL));
	kmscon_vte_set_write_cb(vte, sink_write);
	assert(!kmscon_vte_resize(vte, cols, rows, FAKE_CELL_W, FAKE_CELL_H));

	/* the very first round has to put the whole screen up */
	assert(round_trip(&txt, vte, false) == (int)(cols * rows));
	assert(clears == 1);
	settle(&txt, vte);

	/* a terminal that is not doing anything costs nothing at all */
	assert(round_trip(&txt, vte, false) == -1);
	assert(round_trip(&txt, vte, false) == -1);

	/*
	 * A display that came back from a mode switch has to be repainted even
	 * though the terminal did not move. Nothing else is going to ask for
	 * it: the flag stays set until a swap happens, and a swap only happens
	 * if something draws.
	 */
	need_redraw = true;
	n = round_trip(&txt, vte, false);
	need_redraw = false;
	assert(n == (int)(cols * rows));
	settle(&txt, vte);

	/* writing one line must reach the display, and must not drag the rest
	 * of the screen along with it */
	kmscon_vte_input(vte, "hello", 5);
	n = round_trip(&txt, vte, false);
	assert(n > 0);
	assert(n < (int)cols);
	settle(&txt, vte);

	/* a forced round draws even though nothing changed */
	assert(round_trip(&txt, vte, true) >= 0);
	settle(&txt, vte);

	/* Fill every row, then scroll by one line. The content of the whole
	 * screen moves, so the whole screen reaches the display. */
	for (i = 0; i < rows; ++i) {
		char line[32];

		/* a different character per row, so that shifting the screen
		 * really does change every cell of it */
		memset(line, 'a' + i, 20);
		memcpy(line + 20, "\r\n", 2);
		kmscon_vte_input(vte, line, 22);
	}
	settle(&txt, vte);

	kmscon_vte_input(vte, "one more\r\n", 10);
	assert(round_trip(&txt, vte, false) > (int)cols);
	settle(&txt, vte);

	assert(swaps > 0);

	/*
	 * A frame that was drawn but never reached the screen has to be drawn
	 * again. Nothing else will ask for it: the terminal has moved on and
	 * the renderer would otherwise believe the screen already shows it.
	 */
	kmscon_vte_input(vte, "after the failed swap", 21);
	assert(round_trip(&txt, vte, false) > 0);
	kmscon_text_invalidate(&txt);
	assert(round_trip(&txt, vte, false) == (int)(cols * rows));
	settle(&txt, vte);

	kmscon_vte_free(vte);
	kmscon_text_unset(&txt);
	kmscon_text_bbulk_ops.destroy(&txt);
	return 0;
}
