/*
 * kmscon - PC Screen font backend
 *
 * Copyright (c) awsq.code <awsq.code@gmail.com>
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
 *
 * NOTICE:
 * Code may include fragments by David Herrman <dh.herrmann@googlemail.com>
 * licensed under MIT License
 */

/**
 * SECTION:font_psf.c
 * @short_description: PC Screen font
 * @include: font.h
 *
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "font.h"
#include "shl/log.h"

#define LOG_SUBSYSTEM "font_psf"

#define IS_GZ_MAGIC(m) (m[0] == 0x1f && m[1] == 0x8B)
#define IS_PSF1_MAGIC(m) (m[0] == 0x36 && m[1] == 0x04)
#define IS_PSF2_MAGIC(m) (m[0] == 0x72 && m[1] == 0xb5 && m[2] == 0x4a && m[3] == 0x86)

#define PSF1_MODE512 0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_STARTSEQ 0xfffe
#define PSF1_SEPARATOR 0xffff

#define PSF2_HAS_UNICODE_TABLE 0x01
#define PSF2_STARTSEQ 0xfe
#define PSF2_SEPARATOR 0xff

#define FREAD(f, to, size, err, ...)                                                               \
	if (_fread(f, to, size) < (size)) {                                                        \
		log_error(err);                                                                    \
		__VA_ARGS__                                                                        \
		goto err_file;                                                                     \
	}

typedef int (*fseek_t)(void *, long int, int);
typedef int (*fread_t)(void *, void *, unsigned);
typedef int (*fclose_t)(void *);

typedef struct {
	uint32_t cp;
	uint32_t glyph;
} psf_map_t;

typedef struct {
	psf_map_t *entries;
	uint32_t len;
	uint32_t cap;
} psf_maptab_t;

typedef struct {
	uint32_t glyphs;
	uint32_t step;
	uint32_t height;
	uint32_t width;
	uint32_t scale;
	/* codepoint to glyph index, sorted by codepoint, NULL without a table */
	psf_map_t *map;
	uint32_t map_len;
	uint8_t data[];
} psf_font_t;

static int fread_(void *src, void *dst, unsigned size)
{
	return fread(dst, 1, size, src);
}

/* The table sits after the glyphs and runs to the end, whose offset gzip
 * cannot be asked for, so it is read until the stream dries up. */
static uint8_t *read_rest(void *f, fread_t _fread, size_t *out_len)
{
	size_t cap = 4096, len = 0;
	uint8_t *buf, *tmp;
	int ret;

	buf = malloc(cap);
	if (!buf)
		return NULL;

	while (true) {
		if (len == cap) {
			cap *= 2;
			tmp = realloc(buf, cap);
			if (!tmp) {
				free(buf);
				return NULL;
			}
			buf = tmp;
		}

		ret = _fread(f, buf + len, cap - len);
		if (ret <= 0)
			break;
		len += ret;
	}

	*out_len = len;
	return buf;
}

static int utf8_next(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out)
{
	uint8_t lead = buf[*pos];
	unsigned int extra, i;
	uint32_t cp;

	if (lead < 0x80) {
		extra = 0;
		cp = lead;
	} else if ((lead & 0xe0) == 0xc0) {
		extra = 1;
		cp = lead & 0x1f;
	} else if ((lead & 0xf0) == 0xe0) {
		extra = 2;
		cp = lead & 0x0f;
	} else if ((lead & 0xf8) == 0xf0) {
		extra = 3;
		cp = lead & 0x07;
	} else {
		return -EINVAL;
	}

	if (*pos + extra >= len)
		return -EINVAL;

	for (i = 1; i <= extra; i++) {
		if ((buf[*pos + i] & 0xc0) != 0x80)
			return -EINVAL;
		cp = (cp << 6) | (buf[*pos + i] & 0x3f);
	}

	*pos += extra + 1;
	*out = cp;
	return 0;
}

static int maptab_add(psf_maptab_t *tab, uint32_t cp, uint32_t glyph)
{
	psf_map_t *tmp;

	if (tab->len == tab->cap) {
		tab->cap = tab->cap ? tab->cap * 2 : 256;
		tmp = realloc(tab->entries, tab->cap * sizeof(*tmp));
		if (!tmp)
			return -ENOMEM;
		tab->entries = tmp;
	}

	tab->entries[tab->len].cp = cp;
	tab->entries[tab->len].glyph = glyph;
	tab->len++;
	return 0;
}

/* Each glyph owns a run of UTF-8 codepoints closed by 0xff. A 0xff is never a
 * UTF-8 continuation byte, so the runs stay unambiguous. */
static int parse_psf2_table(psf_maptab_t *tab, uint32_t glyphs, const uint8_t *buf, size_t len)
{
	size_t pos = 0;
	uint32_t glyph, cp;

	for (glyph = 0; glyph < glyphs && pos < len; glyph++) {
		while (pos < len && buf[pos] != PSF2_SEPARATOR) {
			/* multi-codepoint sequences have no single glyph */
			if (buf[pos] == PSF2_STARTSEQ) {
				while (pos < len && buf[pos] != PSF2_SEPARATOR)
					pos++;
				break;
			}

			if (utf8_next(buf, len, &pos, &cp))
				return -EINVAL;
			if (maptab_add(tab, cp, glyph))
				return -ENOMEM;
		}
		pos++;
	}

	return 0;
}

/* Same shape, but the codepoints are little-endian UCS-2 */
static int parse_psf1_table(psf_maptab_t *tab, uint32_t glyphs, const uint8_t *buf, size_t len)
{
	size_t pos = 0;
	uint32_t glyph;
	uint16_t cp;

	for (glyph = 0; glyph < glyphs && pos + 1 < len; glyph++) {
		while (pos + 1 < len) {
			cp = buf[pos] | (buf[pos + 1] << 8);
			pos += 2;

			if (cp == PSF1_SEPARATOR)
				break;

			if (cp == PSF1_STARTSEQ) {
				while (pos + 1 < len) {
					cp = buf[pos] | (buf[pos + 1] << 8);
					pos += 2;
					if (cp == PSF1_SEPARATOR)
						break;
				}
				break;
			}

			if (maptab_add(tab, cp, glyph))
				return -ENOMEM;
		}
	}

	return 0;
}

static int map_cmp(const void *a, const void *b)
{
	const psf_map_t *x = a;
	const psf_map_t *y = b;

	return (x->cp > y->cp) - (x->cp < y->cp);
}

static bool psf_lookup(const psf_font_t *font, uint32_t ch, uint32_t *idx)
{
	psf_map_t key = { .cp = ch };
	psf_map_t *hit;

	if (!font->map)
		return false;

	hit = bsearch(&key, font->map, font->map_len, sizeof(*font->map), map_cmp);
	if (!hit)
		return false;

	*idx = hit->glyph;
	return true;
}

/* Read and index the table, leaving the font unmapped if anything is off so
 * the caller falls back to treating the codepoint as the glyph index. */
static void load_unicode_table(psf_font_t *font, void *f, fread_t _fread, bool psf2)
{
	psf_maptab_t tab = { 0 };
	uint32_t i, uniq = 0;
	uint8_t *buf;
	size_t len;
	int ret;

	buf = read_rest(f, _fread, &len);
	if (!buf) {
		log_warning("failed read unicode table, falling back to raw indices");
		return;
	}

	ret = psf2 ? parse_psf2_table(&tab, font->glyphs, buf, len)
		   : parse_psf1_table(&tab, font->glyphs, buf, len);
	free(buf);

	if (ret || !tab.len) {
		log_warning("unusable unicode table, falling back to raw indices");
		free(tab.entries);
		return;
	}

	qsort(tab.entries, tab.len, sizeof(*tab.entries), map_cmp);

	/* a codepoint listed twice keeps whichever glyph sorted first */
	for (i = 0; i < tab.len; i++) {
		if (i && tab.entries[i].cp == tab.entries[uniq - 1].cp)
			continue;
		tab.entries[uniq++] = tab.entries[i];
	}

	font->map = tab.entries;
	font->map_len = uniq;

	log_debug("unicode table: %d codepoints over %d glyphs", uniq, font->glyphs);
}

static int kmscon_font_psf_init(struct kmscon_font *out, const struct kmscon_font_attr *attr)
{
	unsigned char magic[4];
	psf_font_t *font = NULL;
	uint32_t glyphs, step, height, width;
	uint32_t headersize, flags;
	bool psf2, has_table;

	fseek_t _fseek = (fseek_t)fseek;
	fread_t _fread = (fread_t)fread_;
	fclose_t _fclose = (fclose_t)fclose;

	void *font_file = fopen(attr->name, "rb");
	if (!font_file) {
		log_error("failed open psf font: %s", attr->name);
		return 1;
	}

	FREAD(font_file, &magic, 4, "failed read magic");

	if (IS_GZ_MAGIC(magic)) {
		_fseek(font_file, 0, SEEK_SET);
		font_file = gzdopen(fileno(font_file), "rb");
		if (!font_file) {
			log_error("failed open font as gz: %s", attr->name);
			return 1;
		}
		_fseek = (fseek_t)gzseek;
		_fread = (fread_t)gzread;
		_fclose = (fclose_t)gzclose;
		FREAD(font_file, &magic, 4, "failed read magic");
	}

	if (IS_PSF1_MAGIC(magic)) {
		psf2 = false;
		glyphs = (magic[2] & PSF1_MODE512) ? 512 : 256;
		width = 8;
		height = magic[3];
		step = height;
		has_table = magic[2] & PSF1_MODEHASTAB;
	} else if (IS_PSF2_MAGIC(magic)) {
		psf2 = true;
		_fseek(font_file, 8, SEEK_SET);
		FREAD(font_file, &headersize, 4, "failed read headersize");
		FREAD(font_file, &flags, 4, "failed read flags");
		FREAD(font_file, &glyphs, 4, "failed read glyphs");
		FREAD(font_file, &step, 4, "failed read step");
		FREAD(font_file, &height, 4, "failed read height");
		FREAD(font_file, &width, 4, "failed read width");
		has_table = flags & PSF2_HAS_UNICODE_TABLE;
		_fseek(font_file, headersize, SEEK_SET);
	} else {
		log_error("file isn't psf1 or psf2");
		goto err_file;
	}

	font = malloc(sizeof(*font) + step * glyphs);
	if (!font) {
		log_error("failed allocate font data");
		goto err_file;
	}
	font->glyphs = glyphs;
	font->step = step;
	font->height = height;
	font->width = width;
	font->map = NULL;
	font->map_len = 0;

	FREAD(font_file, font->data, glyphs * step, "file is too short to store all font glyphs",
	      free(font);)

	if (has_table)
		load_unicode_table(font, font_file, _fread, psf2);

	_fclose(font_file);

	memcpy(out->attr.name, attr->name, strlen(attr->name));

	font->scale = (attr->height + (height / 2)) / height;
	if (!font->scale)
		font->scale = 1;
	out->data = font;

	out->attr.bold = false;
	out->attr.italic = false;
	out->attr.width = width * font->scale;
	out->attr.height = height * font->scale;
	out->increase_step = height;

	log_notice("using font: %s %dx%d, scale %d glyphs %d", attr->name, width, height,
		   font->scale, font->glyphs);

	return 0;

err_file:
	_fclose(font_file);
	return 1;
}

static void kmscon_font_psf_destroy(struct kmscon_font *kfont)
{
	psf_font_t *font = kfont->data;

	log_debug("unloading psf font");
	free(font->map);
	free(font);
}

static uint32_t apply_attr(uint32_t c, const struct kmscon_font_attr *attr, bool last_line)
{
	if (attr->bold)
		c |= c >> 1;
	if (attr->underline && last_line)
		c = 0xffffffff;
	return c;
}

static uint8_t unfold(uint32_t val)
{
	return 0xff * !!val;
}

static uint32_t readrow(const uint8_t *data, uint8_t width)
{
	uint32_t row = 0;
	uint8_t len = (width + 7) / 8;

	for (uint8_t i = 0; i < len; i++)
		row = (row << 8) | data[i];

	return row >> (len * 8 - width);
}

static struct kmscon_glyph *new_glyph(uint32_t idx, const struct kmscon_font *kfont)
{
	struct kmscon_glyph *glyph;
	unsigned int w = kfont->attr.width;
	unsigned int h = kfont->attr.height;
	psf_font_t *font = kfont->data;
	uint8_t *glyph_data = font->data + idx * font->step;
	uint32_t c;
	int i, j, k, l;

	glyph = malloc(sizeof(*glyph) + w * h);
	if (!glyph) {
		log_error("failed allocate memory for glyph");
		return NULL;
	}

	glyph->double_width = false;
	glyph->buf.width = w;
	glyph->buf.height = h;

	for (i = 0; i < h; i++) {
		k = i / font->scale;
		c = apply_attr(readrow(glyph_data + k * (font->step / font->height), font->width),
			       &kfont->attr, k == (font->height - 1));

		for (j = 0; j < w; j++) {
			l = j / font->scale;
			glyph->buf.data[i * glyph->buf.width + j] =
				unfold(c & (1 << (font->width - 1 - l)));
		}
	}

	if (kfont->attr.cursor_bar)
		kmscon_glyph_draw_vbar(&glyph->buf, kfont->attr.width);

	return glyph;
}

static bool kmscon_font_psf_has_glyph(struct kmscon_font *kfont, uint32_t ch)
{
	psf_font_t *font = kfont->data;
	uint32_t idx;

	if (font->map)
		return psf_lookup(font, ch, &idx);

	return (ch == FONT_FULL_BLOCK || ch == FONT_VBAR || ch < font->glyphs);
}

static struct kmscon_glyph *kmscon_font_psf_render(struct kmscon_font *kfont, uint32_t ch)
{
	psf_font_t *font = kfont->data;
	uint32_t idx;

	if (font->map) {
		/* the caller walks its own fallbacks when nothing matches */
		if (!psf_lookup(font, ch, &idx))
			return NULL;

		return new_glyph(idx, kfont);
	}

	/* Without a table the codepoint is the glyph index, which only lines up
	 * for ASCII; the block characters come from their CP437 slots. */
	if (ch == FONT_FULL_BLOCK)
		ch = 219;
	else if (ch == FONT_VBAR)
		ch = 179;
	if (ch >= font->glyphs)
		return new_glyph('?', kfont);

	return new_glyph(ch, kfont);
}

struct kmscon_font_ops kmscon_font_psf_ops = {
	.name = "psf",
	.owner = NULL,
	.init = kmscon_font_psf_init,
	.destroy = kmscon_font_psf_destroy,
	.has_glyph = kmscon_font_psf_has_glyph,
	.render = kmscon_font_psf_render,
};
