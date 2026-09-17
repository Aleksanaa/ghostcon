/*
 * Lightweight test for repeated bbulk_set calls (no leaks, all cells re-damaged).
 * We include the implementation to access static helpers.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../src/render/text.h"

/* ---- Stubs for external dependencies used by text_bbulk.c ---- */
#include "../src/font/font.h"	/* for kmscon_font_* */
#include "../src/video/video.h" /* for display_* prototypes */
unsigned int display_get_width(struct display *disp)
{
	(void)disp;
	return 640;
}
unsigned int display_get_height(struct display *disp)
{
	(void)disp;
	return 480;
}

/* Keep geometry tiny to minimize allocations */
#define FAKE_CELL_W 8
#define FAKE_CELL_H 16

/* Stub font metrics APIs used indirectly by text.c/text_bbulk.c */
unsigned int kmscon_font_get_width(const struct kmscon_font *font)
{
	(void)font;
	return FAKE_CELL_W;
}
unsigned int kmscon_font_get_height(const struct kmscon_font *font)
{
	(void)font;
	return FAKE_CELL_H;
}

/* Stub font rendering APIs used by text_bbulk.c */
struct kmscon_glyph *kmscon_font_render(struct kmscon_font *font, const uint32_t ch)
{
	struct kmscon_glyph *g;
	(void)font;
	(void)ch;
	g = malloc(sizeof(*g) + FAKE_CELL_W * FAKE_CELL_W);
	memset(g, 0, sizeof(*g) + FAKE_CELL_W * FAKE_CELL_W);
	g->buf.width = g->buf.height = FAKE_CELL_W;
	return g;
}

bool kmscon_font_has_glyph(struct kmscon_font *font, const uint32_t ch)
{
	(void)font;
	(void)ch;
	return true;
}

int kmscon_rotate_glyph(struct kmscon_glyph *vb, const struct kmscon_glyph *glyph,
			enum Orientation o, uint8_t align)
{
	(void)o;
	(void)align;
	if (!vb || !glyph)
		return -EINVAL;
	*vb = *glyph;
	if (glyph->buf.width == 0 || glyph->buf.height == 0)
		return 0;
	size_t buf_size = glyph->buf.width * glyph->buf.height;
	memset(vb->buf.data, 0x11, buf_size);
	return 0;
}

/* Stub uterm display APIs used by text_bbulk.c */
bool display_need_redraw(struct display *disp)
{
	(void)disp;
	return false;
}
bool display_has_damage(struct display *disp)
{
	(void)disp;
	return false;
}
bool display_supports_damage(struct display *disp)
{
	(void)disp;
	return true;
}
int display_clear(struct display *disp, uint8_t r, uint8_t g, uint8_t b)
{
	(void)disp;
	(void)r;
	(void)g;
	(void)b;
	return 0;
}
int display_blend(struct display *disp, const struct video_blend_req *req)
{
	(void)disp;
	(void)req;
	return 0;
}
void display_set_damage(struct display *disp, size_t n_rect, struct video_rect *damages)
{
	(void)disp;
	(void)n_rect;
	(void)damages;
}
void display_set_cursor_offset(struct display *disp, int32_t x, int32_t y)
{
	(void)disp;
	(void)x;
	(void)y;
}
#include "shl/log.h"
#undef log_warning
#define log_warning(f, ...)
#undef log_debug
#define log_debug(f, ...)
/* ---- The frame that the vte would hand out ---- */
static unsigned int fake_rows;
static unsigned int fake_cols;
static unsigned int fake_next;
static bool fake_dirty[64];
static unsigned int rows_pulled;

bool kmscon_vte_frame_row(struct kmscon_vte *vte, unsigned int *y, bool *dirty)
{
	(void)vte;
	if (fake_next >= fake_rows)
		return false;

	*y = fake_next++;
	if (dirty)
		*dirty = fake_dirty[*y];
	return true;
}

void kmscon_vte_frame_cells(struct kmscon_vte *vte, struct kmscon_cell *cells, unsigned int len)
{
	(void)vte;
	(void)len;
	memset(cells, 0, fake_cols * sizeof(*cells));
	rows_pulled++;
}

/* Pull in the implementation so we can call bbulk_set directly */
#include "../src/render/bbulk.c"

/* Run one frame in which the terminal itself did not change at all */
static unsigned int draw_clean_frame(struct kmscon_text *txt)
{
	fake_cols = txt->cols;
	fake_rows = txt->rows;
	fake_next = 0;
	memset(fake_dirty, 0, sizeof(fake_dirty));
	rows_pulled = 0;
	bbulk_draw(txt);
	return rows_pulled;
}

/* Fake font objects with valid width/height for FONT_WIDTH/FONT_HEIGHT macros */
static struct kmscon_font fake_font = {.attr = {.width = FAKE_CELL_W, .height = FAKE_CELL_H}};

static void init_fake_txt(struct kmscon_text *txt)
{
	memset(txt, 0, sizeof(*txt));
	txt->ops = &kmscon_text_bbulk_ops;
	txt->disp = (struct display *)0x1; /* non-null stub */
	txt->font = &fake_font;
	txt->orientation = OR_NORMAL;
	txt->ref = 1;
}

int main(void)
{
	struct kmscon_text txt;
	int ret;

	init_fake_txt(&txt);

	ret = kmscon_text_bbulk_ops.init(&txt);
	assert(ret == 0);
	struct bbulk *bb = txt.data;
	assert(bb != NULL);

	/* First call allocates */
	ret = bbulk_set(&txt);
	assert(ret == 0);
	unsigned int prev_cells = bb->cell_count;

	bbulk_unset(&txt);

	/* Second call with identical geometry should remain valid and fully damaged */
	ret = bbulk_set(&txt);
	assert(ret == 0);
	assert(bb->cell_count == prev_cells);
	assert(bb->cells != NULL);
	assert(bb->cell_flags != NULL);
	assert(bb->damage_rects != NULL);
	/* All cells should be marked damaged */
	for (unsigned i = 0; i < bb->cell_count; ++i)
		assert(bb->cells[i].ch == ID_DAMAGED);

	/* Exercise prepare/render + damage path */
	struct kmscon_screen_attr attr;
	memset(&attr, 0, sizeof(attr));
	/* every row owes a blit right after a set, so prepare asks for a draw
	 * even though the terminal has not moved */
	txt.frame.attr = attr;
	ret = bbulk_prepare(&txt);
	assert(ret == 1);
	ret = bbulk_render(&txt);
	assert(ret == 0);
	assert(bb->damage_rect_len > 0);

	/*
	 * A row the terminal did not touch is pulled only while it still owes
	 * the off-screen buffer a blit: once to push the new content, once for
	 * the other buffer of the pair, and from then on not at all.
	 */
	assert(draw_clean_frame(&txt) == txt.rows);
	assert(draw_clean_frame(&txt) == txt.rows);
	assert(draw_clean_frame(&txt) == txt.rows);
	assert(draw_clean_frame(&txt) == 0);
	assert(bbulk_prepare(&txt) == 0);

	/* a row the terminal did change is pulled again */
	fake_cols = txt.cols;
	fake_rows = txt.rows;
	fake_next = 0;
	memset(fake_dirty, 0, sizeof(fake_dirty));
	fake_dirty[2] = true;
	rows_pulled = 0;
	bbulk_draw(&txt);
	assert(rows_pulled == 1);

	bbulk_unset(&txt);
	assert(bb->cells == NULL);
	assert(bb->cell_flags == NULL);
	assert(bb->damage_rects == NULL);
	kmscon_text_bbulk_ops.destroy(&txt);
	return 0;
}
