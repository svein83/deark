// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// MacPaint image format

#include <deark-config.h>
#include <deark-modules.h>
#include "fmtutil.h"

#define MACPAINT_WIDTH 576
#define MACPAINT_HEIGHT 720
#define MACPAINT_IMAGE_BYTES ((MACPAINT_WIDTH/8)*MACPAINT_HEIGHT)

typedef struct localctx_struct {
	int has_macbinary_header;
} lctx;

static void do_read_bitmap(deark *c, lctx *d, de_int64 pos)
{
	de_int64 ver_num;
	dbuf *unc_pixels = NULL;

	ver_num = de_getui32be(pos);
	de_dbg(c, "version number: %u\n", (unsigned int)ver_num);
	if(ver_num!=0 && ver_num!=2 && ver_num!=3) {
		de_warn(c, "Unrecognized version number: %u\n", (unsigned int)ver_num);
	}

	pos += 512;

	unc_pixels = dbuf_create_membuf(c, MACPAINT_IMAGE_BYTES);
	dbuf_set_max_length(unc_pixels, MACPAINT_IMAGE_BYTES);

	de_fmtutil_uncompress_packbits(c->infile, pos, c->infile->len - pos, unc_pixels, NULL);

	if(unc_pixels->len < MACPAINT_IMAGE_BYTES) {
		de_warn(c, "Image decompressed to %d bytes, expected %d.\n",
			(int)unc_pixels->len, (int)MACPAINT_IMAGE_BYTES);
	}

	de_convert_and_write_image_bilevel(unc_pixels, 0,
		MACPAINT_WIDTH, MACPAINT_HEIGHT, MACPAINT_WIDTH/8,
		DE_CVTF_WHITEISZERO, NULL);

	dbuf_close(unc_pixels);
}

// A function to help determine if the file has a MacBinary header.
// Each row is RLE-compressed independently, so once we assume one possibility
// or the other, we can do sanity checks to see if any code crosses a row
// boundary, or the image is too small to be a MacPaint image.
// It's inefficient to decompress whole image -- twice -- just to try to
// figure this out, but hopefully it's pretty reliable.
// Returns an integer (0, 1, 2) reflecting the likelihood that this is the
// correct position.
static int valid_file_at(deark *c, lctx *d, de_int64 pos1)
{
	de_byte b;
	de_int64 x;
	de_int64 xpos, ypos;
	de_int64 pos;
	de_int64 imgstart;

	imgstart = pos1+512;

	// Minimum bytes per row is 2.
	// For a valid (non-truncated) file, file size must be at least
	// pos1 + 512 + 2*MACPAINT_HEIGHT. But we want to tolerate truncated
	// files as well.
	if(c->infile->len < imgstart + 4) {
		de_dbg(c, "file too small\n");
		return 0;
	}

	xpos=0; ypos=0;
	pos = pos1 + 512;

	while(pos < c->infile->len) {
		if(ypos>=MACPAINT_HEIGHT) {
			break;
		}

		b = de_getbyte(pos);
		pos++;

		if(b<=127) {
			x = 1+(de_int64)b;
			pos+=x;
			xpos+=8*x;
			if(xpos==MACPAINT_WIDTH) {
				xpos=0;
				ypos++;
			}
			else if(xpos>MACPAINT_WIDTH) {
				de_dbg(c, "image at offset %d: literal too long\n", (int)imgstart);
				return 0;
			}
		}
		else if(b>=129) {
			x = 257 - (de_int64)b;
			pos++;
			xpos+=8*x;
			if(xpos==MACPAINT_WIDTH) {
				xpos=0;
				ypos++;
			}
			else if(xpos>MACPAINT_WIDTH) {
				de_dbg(c, "image at offset %d: run too long\n", (int)imgstart);
				return 0;
			}
		}
	}

	if(xpos==0 && ypos==MACPAINT_HEIGHT) {
		de_dbg(c, "image at offset %d decodes okay\n", (int)imgstart);
		return 2;
	}

	de_dbg(c, "image at offset %d: premature end of file (x=%d, y=%d)\n", (int)imgstart, (int)xpos, (int)ypos);
	return 1;
}

// Some MacPaint files contain a collection of brush patterns.
// Essentially, MacPaint saves workspace settings inside image files.
// (But these patterns are the only setting.)
static void do_read_patterns(deark *c, lctx *d, de_int64 pos)
{
	de_int64 cell;
	de_int64 i, j;
	de_byte x;
	const de_int64 dispwidth = 19;
	const de_int64 dispheight = 17;
	de_int64 xpos, ypos;
	int nonblank = 0;
	struct deark_bitmap *pat = NULL;

	pos += 4;
	pat = de_bitmap_create(c, (dispwidth+1)*19+1, (dispheight+1)*2+1, 1);

	for(cell=0; cell<38; cell++) {
		xpos = (dispwidth+1)*(cell%19)+1;
		ypos = (dispheight+1)*(cell/19)+1;

		for(j=0; j<dispheight; j++) {
			for(i=0; i<dispwidth; i++) {
				// TODO: Figure out the proper "brush origin" of these patterns.
				// Some of them may be shifted differently than MacPaint displays them.
				x = de_get_bits_symbol(c->infile, 1, pos+cell*8+j%8, i%8);

				// 0 = white. Only need to set the white pixels, since they start out
				// black.
				if(x==0) {
					de_bitmap_setpixel_gray(pat, xpos+i, ypos+j, 255);
				}
				else {
					nonblank = 1;
				}
			}
		}
	}

	if(nonblank) {
		de_bitmap_write_to_file(pat, "pat");
	}
	de_bitmap_destroy(pat);
}

static void de_run_macpaint(deark *c, const char *params)
{
	lctx *d;
	de_int64 pos;
	const char *s;

	de_dbg(c, "In macpaint module\n");

	d = de_malloc(c, sizeof(lctx));

	d->has_macbinary_header = -1;

	s = de_get_ext_option(c, "macpaint:macbinary");
	if(s) d->has_macbinary_header = de_atoi(s);

	if(d->has_macbinary_header == -1) {
		int v512;
		int v640;
		de_dbg(c, "trying to determine if file has a MacBinary header\n");
		v512 = valid_file_at(c, d, 0);
		v640 = valid_file_at(c, d, 128);
		if(v512 > v640) {
			de_dbg(c, "assuming it has no MacBinary header\n");
			d->has_macbinary_header = 0;
		}
		else if(v640 > v512) {
			de_dbg(c, "assuming it has a MacBinary header\n");
			d->has_macbinary_header = 1;
		}
		else if(v512 && v640) {
			de_warn(c, "Can't determine if this file has a MacBinary header. "
				"Try \"-opt macpaint:macbinary=0\".\n");
			d->has_macbinary_header = 1;
		}
		else {
			de_warn(c, "This is probably not a MacPaint file.\n");
			d->has_macbinary_header = 1;
		}
	}

	if(d->has_macbinary_header)
		de_declare_fmt(c, "MacPaint with MacBinary header");
	else
		de_declare_fmt(c, "MacPaint without MacBinary header");

	pos = d->has_macbinary_header ? 128 : 0;

	do_read_bitmap(c, d, pos);

	if(c->extract_level>=2) {
		do_read_patterns(c, d, pos);
	}

	de_free(c, d);
}

static int de_identify_macpaint(deark *c)
{
	de_byte buf[8];

	de_read(buf, 65, 8);

	// Not all MacPaint files can be easily identified, but this will work
	// for some of them.
	if(!de_memcmp(buf, "PNTGMPNT", 8)) return 80;
	if(!de_memcmp(buf, "PNTG", 4)) return 70;

	if(de_input_file_has_ext(c, "mac")) return 10;
	return 0;
}

void de_module_macpaint(deark *c, struct deark_module_info *mi)
{
	mi->id = "macpaint";
	mi->run_fn = de_run_macpaint;
	mi->identify_fn = de_identify_macpaint;
}
