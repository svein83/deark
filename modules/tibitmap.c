// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	de_int64 w, h;
} lctx;

static void do_bitmap(deark *c, lctx *d)
{
	struct deark_bitmap *img = NULL;
	de_int64 j;
	de_int64 pos;
	de_int64 rowspan;

	// This decoder is based on reverse engineering, and may not be correct.

	d->h = de_getui16be(88);
	d->w = de_getui16be(90);
	if(!de_good_image_dimensions(c, d->w, d->h)) goto done;
	img = de_bitmap_create(c, d->w, d->h, 1);

	rowspan = (d->w+7)/8;

	pos = 92;
	for(j=0; j<d->h; j++) {
		de_convert_row_bilevel(c->infile, pos+j*rowspan, img, j, DE_CVTR_WHITEISZERO);
	}

	de_bitmap_write_to_file(img, NULL);
done:
	de_bitmap_destroy(img);
}

static void de_run_tibitmap(deark *c, const char *params)
{
	lctx *d = NULL;

	de_dbg(c, "In tibitmap module\n");
	d = de_malloc(c, sizeof(lctx));
	do_bitmap(c, d);
	de_free(c, d);
}

static int de_identify_tibitmap(deark *c)
{
	de_byte buf[8];
	de_read(buf, 0, 8);

	if(!de_memcmp(buf, "**TI92**", 8)) return 100;
	if(!de_memcmp(buf, "**TI89**", 8)) return 100;
	return 0;
}

void de_module_tibitmap(deark *c, struct deark_module_info *mi)
{
	mi->id = "tibitmap";
	mi->run_fn = de_run_tibitmap;
	mi->identify_fn = de_identify_tibitmap;
}
