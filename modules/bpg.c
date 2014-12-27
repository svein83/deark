// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

#include <deark-config.h>
#include <deark-modules.h>
#include "fmtutil.h"

// BPG

typedef struct localctx_struct {
	de_int64 width, height;

	de_byte pixel_format;
	de_byte alpha_flag;
	de_int64 bit_depth;

	de_byte color_space;
	de_byte extension_present_flag;
	de_byte alpha2_flag;
	de_byte limited_range_flag;

	de_int64 picture_data_len;
	de_int64 extension_data_len;

} lctx;

static de_int64 get_ue7(deark *c, de_int64 *pos)
{
	de_byte b;
	de_int64 val = 0;
	int bytecount = 0;

	// TODO: Better error handling
	while(1) {
		b = de_getbyte(*pos);
		(*pos)++;
		bytecount++;

		// A quick hack to prevent 64-bit integer overflow.
		// 5 bytes are enough for 35 bits, and none of the fields in
		// BPG v0.9.4.1 require more than 32.
		if(bytecount<=5)
			val = (val<<7)|(b&0x7f);

		if(b<0x80) {
			break;
		}
	}
	return val;
}

static void do_extensions(deark *c, lctx *d, de_int64 pos)
{
	de_int64 endpos;
	de_int64 tag;
	de_int64 payload_len;

	endpos = pos + d->extension_data_len;

	while(pos < endpos) {
		tag = get_ue7(c, &pos);
		payload_len = get_ue7(c, &pos);
		if(pos+payload_len>endpos) break;

		switch(tag) {
		case 1: // Exif
			de_fmtutil_handle_exif(c, pos, payload_len);
			break;
		case 2: // ICC profile
			dbuf_create_file_from_slice(c->infile, pos, payload_len, "icc", NULL);
			break;
		case 3: // XMP
			dbuf_create_file_from_slice(c->infile, pos, payload_len, "xmp", NULL);
			break;
		case 4: // Thumbnail
			dbuf_create_file_from_slice(c->infile, pos, payload_len, "thumb.bpg", NULL);
			break;
		default:
			de_dbg(c, "unrecognized extension type: %d\n", (int)tag);
		}

		pos += payload_len;
	}
}

static void do_hevc_file(deark *c, lctx *d)
{
	de_int64 pos;
	de_byte b;

	pos = 4;
	b = de_getbyte(pos);
	pos++;
	d->pixel_format = b>>5;
	d->alpha_flag = (b>>4)&0x01;
	d->bit_depth = (de_int64)(b&0x0f) +8;
	de_dbg(c, "pixel format: %d\n", (int)d->pixel_format);
	de_dbg(c, "alpha flag: %d\n", (int)d->alpha_flag);
	de_dbg(c, "bit depth: %d\n", (int)d->bit_depth);

	b = de_getbyte(pos);
	pos++;
	d->color_space = b>>4;
	d->extension_present_flag = (b>>3)&0x01;
	d->alpha2_flag = (b>>2)&0x01;
	d->limited_range_flag = (b>>1)&0x01;
	de_dbg(c, "color_space: %d\n", (int)d->color_space);
	de_dbg(c, "extension_present_flag: %d\n", (int)d->extension_present_flag);
	de_dbg(c, "alpha2_flag: %d\n", (int)d->alpha2_flag);
	de_dbg(c, "limited_range_flag: %d\n", (int)d->limited_range_flag);

	d->width = get_ue7(c, &pos);
	d->height = get_ue7(c, &pos);
	de_dbg(c, "dimensions: %dx%d\n", (int)d->width, (int)d->height);


	d->picture_data_len = get_ue7(c, &pos);
	de_dbg(c, "picture_data_len: %d%s\n", (int)d->picture_data_len,
		(d->picture_data_len==0)?" (= to EOF)":"");

	if(d->extension_present_flag) {
		d->extension_data_len = get_ue7(c, &pos);
		de_dbg(c, "extension data len: %d\n", (int)d->extension_data_len);
	}

	if(d->extension_present_flag) {
		do_extensions(c, d, pos);
		pos += d->extension_data_len;
	}

	de_dbg(c, "hevc_header_and_data begins at %d\n", (int)pos);
}

static void de_run_bpg(deark *c, const char *params)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));

	de_dbg(c, "In bpg module\n");
	do_hevc_file(c, d);

	de_free(c, d);
}

static int de_identify_bpg(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x42\x50\x47\xfb", 4)) {
		return 100;
	}
	return 0;
}

void de_module_bpg(deark *c, struct deark_module_info *mi)
{
	mi->id = "bpg";
	mi->run_fn = de_run_bpg;
	mi->identify_fn = de_identify_bpg;
}