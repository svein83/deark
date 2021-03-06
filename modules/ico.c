// This file is part of Deark.
// Copyright (C) 2016 Jason Summers
// See the file COPYING for terms of use.

// Windows ICO and CUR formats

#include <deark-config.h>
#include <deark-private.h>
#include <deark-fmtutil.h>
DE_DECLARE_MODULE(de_module_ico);

typedef struct localctx_struct {
	int is_cur;
	int extract_unused_masks;
} lctx;

static void do_extract_png(deark *c, lctx *d, i64 pos, i64 len)
{
	char ext[64];
	i64 w, h;

	// Peek at the PNG data, to figure out the dimensions.
	w = de_getu32be(pos+16);
	h = de_getu32be(pos+20);

	de_snprintf(ext, sizeof(ext), "%dx%d.png", (int)w, (int)h);
	dbuf_create_file_from_slice(c->infile, pos, len, ext, NULL, 0);
}

static void do_image_data(deark *c, lctx *d, i64 img_num, i64 pos1, i64 len)
{
	struct de_bmpinfo bi;
	i64 fg_start, bg_start;
	i64 i, j;
	u32 pal[256];
	i64 p;
	de_bitmap *img = NULL;
	u8 x;
	u8 cr=0, cg=0, cb=0, ca=0;
	int inverse_warned = 0;
	int use_mask;
	int has_alpha_channel = 0;
	i64 bitcount_color;
	char filename_token[32];

	if(pos1+len > c->infile->len) return;

	if(!de_fmtutil_get_bmpinfo(c, c->infile, &bi, pos1, len, DE_BMPINFO_ICO_FORMAT)) {
		de_err(c, "Invalid bitmap");
		return;
	}

	if(bi.file_format == DE_BMPINFO_FMT_PNG) {
		do_extract_png(c, d, pos1, len);
		return;
	}

	switch(bi.bitcount) {
	case 1: case 2: case 4: case 8: case 24: case 32:
		break;
	case 16:
		de_err(c, "(image #%d) Unsupported bit count (%d)", (int)img_num, (int)bi.bitcount);
		goto done;
	default:
		de_err(c, "(image #%d) Invalid bit count (%d)", (int)img_num, (int)bi.bitcount);
		goto done;
	}

	if(bi.compression_field!=0) {
		// TODO: Support BITFIELDS
		de_err(c, "Compression / BITFIELDS not supported");
		goto done;
	}

	if(bi.bitcount==32) {
		// 32bpp images have both an alpha channel, and a 1bpp "mask".
		// We never use a 32bpp image's mask (although we may extract it
		// separately).
		// I'm not sure that's necessarily the best thing to do. I think that
		// in theory the mask could be used to get inverted-background-color
		// pixels, though I don't know if Windows allows that.
		use_mask = 0;
		has_alpha_channel = 1;
	}
	else {
		use_mask = 1;
	}

	// In the filename, we use the bitcount just for the color data,
	// ignoring any masks or alpha channel.
	bitcount_color = bi.bitcount;
	if(bi.bitcount==32) bitcount_color = 24;
	de_snprintf(filename_token, sizeof(filename_token), "%dx%dx%d",
		(int)bi.width, (int)bi.height, (int)bitcount_color);

	img = de_bitmap_create(c, bi.width, bi.height, 4);
	img->flipped = 1;

	// Read palette
	de_zeromem(pal, sizeof(pal));
	if (bi.pal_entries > 0) {
		if(bi.pal_entries>256) goto done;

		de_read_palette_rgb(c->infile,
			pos1+bi.infohdrsize, bi.pal_entries, bi.bytes_per_pal_entry,
			pal, 256, DE_GETRGBFLAG_BGR);
	}

	fg_start = pos1 + bi.size_of_headers_and_pal;
	bg_start = pos1 + bi.size_of_headers_and_pal + bi.foreground_size;

	de_dbg(c, "foreground at %d, mask at %d", (int)fg_start, (int)bg_start);

	for(j=0; j<img->height; j++) {
		for(i=0; i<img->width; i++) {

			if(bi.bitcount<=8) {
				p = fg_start + bi.rowspan*j;
				x = de_get_bits_symbol(c->infile, bi.bitcount, p, i);
				cr = DE_COLOR_R(pal[x]);
				cg = DE_COLOR_G(pal[x]);
				cb = DE_COLOR_B(pal[x]);
			}
			//else if(bi.bitcount==16) {
			//	// TODO
			//}
			else if(bi.bitcount==24) {
				p = fg_start + bi.rowspan*j + i*3;
				cb = de_getbyte(p+0);
				cg = de_getbyte(p+1);
				cr = de_getbyte(p+2);
			}
			else if(bi.bitcount==32) {
				p = fg_start + bi.rowspan*j + i*4;
				cb = de_getbyte(p+0);
				cg = de_getbyte(p+1);
				cr = de_getbyte(p+2);
				if(has_alpha_channel) {
					ca = de_getbyte(p+3);
				}
			}

			if(use_mask) {
				// Read the mask bit, if the main bitmap didn't already
				// have transparency.

				p = bg_start + bi.mask_rowspan*j;
				x = de_get_bits_symbol(c->infile, 1, p, i);
				ca = x ? 0 : 255;

				// Inverted background pixels
				// TODO: Should we do this only for cursors, and not icons?
				if(x==1 && (cr || cg || cb)) {
					if(!inverse_warned) {
						de_warn(c, "This image contains inverse background pixels, which are not fully supported.");
						inverse_warned = 1;
					}
					if((i+j)%2) {
						cr = 255; cg = 0; cb=128; ca = 128;
					}
					else {
						cr = 128; cg = 0; cb=255; ca = 128;
					}
				}
			}

			de_bitmap_setpixel_rgba(img, i, j, DE_MAKE_RGBA(cr,cg,cb,ca));
		}
	}

	de_optimize_image_alpha(img, (bi.bitcount==32)?0x1:0x0);

	de_bitmap_write_to_file(img, filename_token, 0);

	if(!use_mask && d->extract_unused_masks) {
		char maskname_token[32];
		de_bitmap *mask_img = NULL;

		de_snprintf(maskname_token, sizeof(maskname_token), "%dx%dmask",
			(int)bi.width, (int)bi.height);
		mask_img = de_bitmap_create(c, bi.width, bi.height, 1);
		mask_img->flipped = 1;
		de_convert_image_bilevel(c->infile, bg_start, bi.mask_rowspan, mask_img, 0);
		de_bitmap_write_to_file(mask_img, maskname_token, DE_CREATEFLAG_IS_AUX);
		de_bitmap_destroy(mask_img);
	}

done:
	de_bitmap_destroy(img);
}

static void do_image_dir_entry(deark *c, lctx *d, i64 img_num, i64 pos)
{
	i64 data_size;
	i64 data_offset;

	de_dbg(c, "image #%d, index at %d", (int)img_num, (int)pos);
	de_dbg_indent(c, 1);
	data_size = de_getu32le(pos+8);
	data_offset = de_getu32le(pos+12);
	de_dbg(c, "offset=%d, size=%d", (int)data_offset, (int)data_size);

	do_image_data(c, d, img_num, data_offset, data_size);

	de_dbg_indent(c, -1);
}

static void de_run_ico(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	i64 x;
	i64 num_images;
	i64 i;

	d = de_malloc(c, sizeof(lctx));
	d->extract_unused_masks = (c->extract_level>=2);

	x = de_getu16le(2);
	if(x==1) {
		d->is_cur=0;
		de_declare_fmt(c, "Windows Icon");
	}
	else if(x==2) {
		d->is_cur=1;
		de_declare_fmt(c, "Windows Cursor");
	}
	else {
		de_dbg(c, "Not an ICO/CUR file");
		goto done;
	}

	num_images = de_getu16le(4);
	de_dbg(c, "images in file: %d", (int)num_images);
	if(!de_good_image_count(c, num_images)) {
		goto done;
	}

	for(i=0; i<num_images; i++) {
		do_image_dir_entry(c, d, i, 6+16*i);
	}

done:
	de_free(c, d);
}

// Windows icons and cursors don't have a distinctive signature. This
// function tries to screen out other formats.
static int is_windows_ico_or_cur(deark *c)
{
	i64 numicons;
	i64 i;
	i64 size, offset;
	u8 buf[4];

	de_read(buf, 0, 4);
	if(de_memcmp(buf, "\x00\x00\x01\x00", 4) &&
		de_memcmp(buf, "\x00\x00\x02\x00", 4))
	{
		return 0;
	}

	numicons = de_getu16le(4);

	// Each icon must use at least 16 bytes for the directory, 40 for the
	// info header, 4 for the foreground, and 4 for the mask.
	if(numicons<1 || (6+numicons*64)>c->infile->len) return 0;

	// Examine the first few icon index entries.
	for(i=0; i<numicons && i<8; i++) {
		size = de_getu32le(6+16*i+8);
		offset = de_getu32le(6+16*i+12);
		if(size<48) return 0;
		if(offset < 6+numicons*16) return 0;
		if(offset+size > c->infile->len) return 0;
	}
	return 1;
}

static int de_identify_ico(deark *c)
{
	if(is_windows_ico_or_cur(c)) {
		return 80;
	}
	return 0;
}

void de_module_ico(deark *c, struct deark_module_info *mi)
{
	mi->id = "ico";
	mi->desc = "Microsoft Windows icon";
	mi->run_fn = de_run_ico;
	mi->identify_fn = de_identify_ico;
}
