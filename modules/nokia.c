// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	de_int64 w, h;
} lctx;

// **************************************************************************
// Nokia Operator Logo (NOL)
//
// Caution: This code is not based on any official specifications.
// **************************************************************************

static void nol_ngg_read_bitmap(deark *c, lctx *d, de_int64 pos)
{
	struct deark_bitmap *img = NULL;
	de_int64 i, j;
	de_byte n;

	img = de_bitmap_create(c, d->w, d->h, 1);

	for(j=0; j<d->h; j++) {
		for(i=0; i<d->w; i++) {
			n = de_getbyte(pos);
			pos++;
			de_bitmap_setpixel_gray(img, i, j, n=='1' ? 0 : 255);
		}
	}

	de_bitmap_write_to_file(img, NULL);
	de_bitmap_destroy(img);
}

static void de_run_nol(deark *c, const char *params)
{
	lctx *d = NULL;

	de_dbg(c, "In NOL module\n");

	d = de_malloc(c, sizeof(lctx));

	d->w = de_getui16le(10);
	d->h = de_getui16le(12);
	if(!de_good_image_dimensions(c, d->w, d->h)) goto done;

	nol_ngg_read_bitmap(c, d, 20);
done:
	de_free(c, d);
}

static int de_identify_nol(deark *c)
{
	de_byte buf[3];
	de_read(buf, 0, 3);

	if(!de_memcmp(buf, "NOL", 3)) return 80;
	return 0;
}

void de_module_nol(deark *c, struct deark_module_info *mi)
{
	mi->id = "nol";
	mi->run_fn = de_run_nol;
	mi->identify_fn = de_identify_nol;
}

// **************************************************************************
// Nokia Group Graphic (NGG)
//
// Caution: This code is not based on any official specifications.
// **************************************************************************

static void de_run_ngg(deark *c, const char *params)
{
	lctx *d = NULL;

	de_dbg(c, "In NGG module\n");

	d = de_malloc(c, sizeof(lctx));

	d->w = de_getui16le(6);
	d->h = de_getui16le(8);
	if(!de_good_image_dimensions(c, d->w, d->h)) goto done;

	nol_ngg_read_bitmap(c, d, 16);
done:
	de_free(c, d);
}

static int de_identify_ngg(deark *c)
{
	de_byte buf[3];
	de_read(buf, 0, 3);

	if(!de_memcmp(buf, "NGG", 3)) return 80;
	return 0;
}

void de_module_ngg(deark *c, struct deark_module_info *mi)
{
	mi->id = "ngg";
	mi->run_fn = de_run_ngg;
	mi->identify_fn = de_identify_ngg;
}
