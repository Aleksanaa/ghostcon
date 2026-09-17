/*
 * kmscon - Text Renderer
 *
 * Copyright (c) 2012-2013 David Herrmann <dh.herrmann@googlemail.com>
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
 * Text Renderer
 * The Text-Renderer subsystem provides a simple way to draw text into a
 * framebuffer. The system is modular and several different backends are
 * available that can be used.
 */

#ifndef KMSCON_TEXT_H
#define KMSCON_TEXT_H

#include <errno.h>
#include <stdlib.h>
#include "font/font.h"
#include "shl/module.h"
#include "video/video.h"
#include "vte.h"

/* text renderer */

enum Orientation {
	OR_NORMAL = 0,	// 0 Degree
	OR_RIGHT,	// 90 Degree
	OR_UPSIDE_DOWN, // 180 Degree
	OR_LEFT,	// 270 Degree
};

struct kmscon_text;
struct kmscon_text_ops;

struct kmscon_text {
	unsigned long ref;
	struct shl_register_record *record;
	const struct kmscon_text_ops *ops;
	void *data;

	struct kmscon_font *font;
	struct display *disp;
	unsigned int cols;
	unsigned int rows;
	unsigned int max_cols;
	unsigned int max_rows;
	bool rendering;
	enum Orientation orientation;

	/* where this renderer stands in the terminal's history of changes,
	 * and the frame it is currently drawing */
	struct kmscon_vte *vte;
	struct kmscon_vte_watch watch;
	struct kmscon_vte_frame frame;
};

struct kmscon_text_ops {
	const char *name;
	struct shl_module *owner;
	int (*init)(struct kmscon_text *txt);
	void (*destroy)(struct kmscon_text *txt);
	int (*set)(struct kmscon_text *txt);
	void (*unset)(struct kmscon_text *txt);
	void (*resize)(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
	int (*rotate)(struct kmscon_text *txt, enum Orientation orientation);
	/*
	 * Returns 1 if the backend has to redraw even though the terminal did
	 * not change, 0 if it is happy to skip a clean frame.
	 *
	 * A clean frame is not drawn and therefore never reaches the display,
	 * so a backend whose buffers went stale for a reason of its own has to
	 * say so here or the screen stays as it was. That covers at least a
	 * display asking for a redraw after a mode switch, a resize, a
	 * rotation, and the frames owed to the second buffer of the pair.
	 */
	int (*prepare)(struct kmscon_text *txt);
	/* Pull txt->vte with kmscon_vte_frame_row() and draw what is needed. */
	int (*draw)(struct kmscon_text *txt);
	int (*draw_pointer)(struct kmscon_text *txt, unsigned int x, unsigned int y);
	int (*render)(struct kmscon_text *txt);
	/* Throw away whatever the backend believes is on screen. */
	void (*invalidate)(struct kmscon_text *txt);
};

#define FONT_WIDTH(txt) ((txt)->font->attr.width)
#define FONT_HEIGHT(txt) ((txt)->font->attr.height)

int kmscon_text_register(const struct kmscon_text_ops *ops);
void kmscon_text_unregister(const char *name);

int kmscon_text_new(struct kmscon_text **out, const char *backend, const char *rotate);
void kmscon_text_ref(struct kmscon_text *txt);
void kmscon_text_unref(struct kmscon_text *txt);

int kmscon_text_set(struct kmscon_text *txt, struct kmscon_font *font, struct display *disp);
void kmscon_text_unset(struct kmscon_text *txt);
unsigned int kmscon_text_get_cols(struct kmscon_text *txt);
unsigned int kmscon_text_get_rows(struct kmscon_text *txt);

enum Orientation kmscon_text_get_orientation(struct kmscon_text *txt);
void kmscon_text_resize(struct kmscon_text *txt, unsigned int cols, unsigned int rows);
int kmscon_text_rotate(struct kmscon_text *txt, enum Orientation orientation);

/* Open a rendering round on @vte. Returns 1 when there is something to draw,
 * 0 when the screen is already up to date and the whole round can be skipped,
 * or a negative error code. @force asks for a frame even if nothing changed. */
int kmscon_text_prepare(struct kmscon_text *txt, struct kmscon_vte *vte, bool force);
int kmscon_text_draw(struct kmscon_text *txt);
int kmscon_text_draw_pointer(struct kmscon_text *txt, unsigned int x, unsigned int y);
int kmscon_text_render(struct kmscon_text *txt);

/* The frame that was just drawn never reached the screen, so forget that it
 * was ever drawn and build the next one from scratch. */
void kmscon_text_invalidate(struct kmscon_text *txt);

/* modularized backends */

extern struct kmscon_text_ops kmscon_text_bbulk_ops;
extern struct kmscon_text_ops kmscon_text_gltex_ops;

#endif /* KMSCON_TEXT_H */
