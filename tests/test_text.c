/*
 * Lightweight test for kmscon_text_set / kmscon_text_unset and the rule that
 * decides whether a rendering round happens at all.
 * We avoid linking the whole tree by stubbing external deps.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "../src/render/text.c" /* pull in kmscon_text_set without changing meson */

/* --- Stubs for the frame the vte hands out --- */
static int fake_changed;
static uint64_t fake_seq;
static int frame_ends;

int kmscon_vte_frame_begin(struct kmscon_vte *vte, const struct kmscon_vte_watch *watch,
			   struct kmscon_vte_frame *out)
{
	memset(out, 0, sizeof(*out));
	out->cols = 2;
	out->rows = 1;
	return fake_changed;
}

bool kmscon_vte_frame_row(struct kmscon_vte *vte, unsigned int *y, bool *dirty)
{
	return false;
}

void kmscon_vte_frame_cells(struct kmscon_vte *vte, struct kmscon_cell *cells, unsigned int len) {}

void kmscon_vte_frame_end(struct kmscon_vte *vte, struct kmscon_vte_watch *watch)
{
	watch->seq = ++fake_seq;
	frame_ends++;
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

/* what the backend answers when asked whether it has work of its own */
static int dummy_prepare_ret;
static int dummy_draws;
static int dummy_prepare(struct kmscon_text *txt)
{
	return dummy_prepare_ret;
}
static int dummy_draw(struct kmscon_text *txt)
{
	dummy_draws++;
	return 0;
}

static struct kmscon_text_ops dummy_ops = {
	.name = "dummytest",
	.owner = NULL,
	.init = NULL,
	.destroy = NULL,
	.set = dummy_set,
	.unset = dummy_unset,
	.prepare = dummy_prepare,
	.draw = dummy_draw,
};

/*
 * A frame is only drawn when somebody needs it: the terminal changed, the
 * backend still has work of its own, or the caller forces one.
 */
static void test_skip_clean_frame(void)
{
	struct kmscon_text txt;
	struct kmscon_font fake_font;
	struct kmscon_vte *fake_vte = (struct kmscon_vte *)0x1;

	memset(&txt, 0, sizeof(txt));
	memset(&fake_font, 0, sizeof(fake_font));
	txt.ops = &dummy_ops;
	txt.font = &fake_font;
	txt.disp = (struct display *)0x1;

	/* nothing changed anywhere, so the whole round is skipped */
	fake_changed = 0;
	dummy_prepare_ret = 0;
	assert(kmscon_text_prepare(&txt, fake_vte, false) == 0);
	assert(!txt.rendering);

	/* the terminal moved */
	fake_changed = 1;
	assert(kmscon_text_prepare(&txt, fake_vte, false) == 1);
	assert(txt.rendering);
	assert(txt.frame.cols == 2);

	/* the backend is behind even though the terminal is not */
	fake_changed = 0;
	dummy_prepare_ret = 1;
	assert(kmscon_text_prepare(&txt, fake_vte, false) == 1);

	/* the caller insists, for instance because it paints a mouse pointer */
	dummy_prepare_ret = 0;
	assert(kmscon_text_prepare(&txt, fake_vte, true) == 1);

	/* drawing a frame moves the watch forward */
	dummy_draws = 0;
	frame_ends = 0;
	assert(kmscon_text_draw(&txt) == 0);
	assert(dummy_draws == 1);
	assert(frame_ends == 1);
	assert(txt.watch.seq == fake_seq);

	/* a renderer bigger than the frame it was handed draws nothing */
	txt.cols = 99;
	assert(kmscon_text_draw(&txt) == -EINVAL);
	assert(dummy_draws == 1);
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

	test_skip_clean_frame();
	test_glyph_vbar();

	return 0;
}
