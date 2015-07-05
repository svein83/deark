// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// This file is for format-specific functions that are used by multiple modules.

#include <deark-config.h>
#include <deark-private.h>
#include "fmtutil.h"

// Gathers information about a DIB.
// If DE_BMPINFO_HAS_FILEHEADER flag is set, pos points to the BITMAPFILEHEADER.
// Otherwise, it points to the BITMAPINFOHEADER.
// Caller allocates bi.
// Returns 0 if BMP is invalid.
int de_fmtutil_get_bmpinfo(deark *c, dbuf *f, struct de_bmpinfo *bi, de_int64 pos,
	de_int64 len, unsigned int flags)
{
	de_int64 fhs; // file header size
	de_int64 bmih_pos;

	de_memset(bi, 0, sizeof(struct de_bmpinfo));

	fhs = (flags & DE_BMPINFO_HAS_FILEHEADER) ? 14 : 0;

	if(fhs+len < 16) return 0;

	if(fhs) {
		if(flags & DE_BMPINFO_HAS_HOTSPOT) {
			bi->hotspot_x = dbuf_getui16le(f, pos+6);
			bi->hotspot_y = dbuf_getui16le(f, pos+8);
			de_dbg(c, "hotspot: (%d,%d)\n", (int)bi->hotspot_x, (int)bi->hotspot_y);
		}

		bi->bitsoffset = dbuf_getui32le(f, pos+10);
		de_dbg(c, "bits offset: %d\n", (int)bi->bitsoffset);
	}

	bmih_pos = pos + fhs;

	bi->infohdrsize = dbuf_getui32le(f, bmih_pos);

	if(bi->infohdrsize==0x474e5089 && (flags & DE_BMPINFO_ICO_FORMAT)) {
		// We don't examine PNG-formatted icons, but we can identify them.
		bi->infohdrsize = 0;
		bi->file_format = DE_BMPINFO_FMT_PNG;
		return 1;
	}

	de_dbg(c, "info header size: %d\n", (int)bi->infohdrsize);

	if(bi->infohdrsize==12) {
		bi->bytes_per_pal_entry = 3;
		bi->width = dbuf_getui16le(f, bmih_pos+4);
		bi->height = dbuf_getui16le(f, bmih_pos+6);
		bi->bitcount = dbuf_getui16le(f, bmih_pos+10);
	}
	else if(bi->infohdrsize>=16 && bi->infohdrsize<=124) {
		bi->bytes_per_pal_entry = 4;
		bi->width = dbuf_getui32le(f, bmih_pos+4);
		bi->height = dbuf_getui32le(f, bmih_pos+8);
		if(bi->height<0) {
			bi->is_topdown = 1;
			bi->height = -bi->height;
		}
		bi->bitcount = dbuf_getui16le(f, bmih_pos+14);
		if(bi->infohdrsize>=20) {
			bi->compression_field = dbuf_getui32le(f, bmih_pos+16);
		}
		if(bi->infohdrsize>=36) {
			bi->pal_entries = dbuf_getui32le(f, bmih_pos+32);
		}
	}
	else {
		return 0;
	}

	if(flags & DE_BMPINFO_ICO_FORMAT) bi->height /= 2;

	if(bi->bitcount>=1 && bi->bitcount<=8) {
		if(bi->pal_entries==0) {
			bi->pal_entries = (de_int64)(1<<(unsigned int)bi->bitcount);
		}
		// I think the NumColors field (in icons) is supposed to be the maximum number of
		// colors implied by the bit depth, not the number of colors in the palette.
		bi->num_colors = (de_int64)(1<<(unsigned int)bi->bitcount);
	}
	else {
		// An arbitrary value. All that matters is that it's >=256.
		bi->num_colors = 16777216;
	}

	de_dbg(c, "image size: %dx%d\n", (int)bi->width, (int)bi->height);
	de_dbg(c, "bit count: %d\n", (int)bi->bitcount);
	de_dbg(c, "palette entries: %d\n", (int)bi->pal_entries);

	bi->pal_bytes = bi->bytes_per_pal_entry*bi->pal_entries;
	bi->size_of_headers_and_pal = fhs + bi->infohdrsize + bi->pal_bytes;
	if(bi->compression_field==3) {
		bi->size_of_headers_and_pal += 12; // BITFIELDS
	}

	if(bi->compression_field==0) {
		// Try to figure out the true size of the resource, minus any padding.

		bi->rowspan = ((bi->bitcount*bi->width +31)/32)*4;
		bi->foreground_size = bi->rowspan * bi->height;

		if(flags & DE_BMPINFO_ICO_FORMAT) {
			bi->mask_rowspan = ((bi->width +31)/32)*4;
			bi->mask_size = bi->mask_rowspan * bi->height;
		}
		else {
			bi->mask_size = 0;
		}

		bi->total_size = bi->size_of_headers_and_pal + bi->foreground_size + bi->mask_size;
	}
	else {
		// Don't try to figure out the true size of compressed or other unusual images.
		bi->total_size = len;
	}

	return 1;
}

void de_fmtutil_handle_exif(deark *c, de_int64 pos, de_int64 len)
{
	dbuf *old_ifile;

	if(c->extract_level>=2) {
		// Writing raw Exif data isn't very useful, but do so if requested.
		dbuf_create_file_from_slice(c->infile, pos, len, "exif.tif", NULL);

		// Caller will have to reprocess the Exif file to extract anything from it.
		return;
	}

	old_ifile = c->infile;

	c->infile = dbuf_open_input_subfile(old_ifile, pos, len);
	de_run_module_by_id(c, "tiff", "E");
	dbuf_close(c->infile);

	c->infile = old_ifile;
}

void de_fmtutil_handle_photoshop_rsrc(deark *c, de_int64 pos, de_int64 len)
{
	dbuf *old_ifile;

	old_ifile = c->infile;

	c->infile = dbuf_open_input_subfile(old_ifile, pos, len);
	de_run_module_by_id(c, "psd", "R");
	dbuf_close(c->infile);

	c->infile = old_ifile;
}

// Returns 0 on failure (currently impossible).
int de_fmtutil_uncompress_packbits(dbuf *f, de_int64 pos1, de_int64 len,
	dbuf *unc_pixels, de_int64 *cmpr_bytes_consumed)
{
	de_int64 pos;
	de_byte b, b2;
	de_int64 count;
	de_int64 endpos;
	de_int64 bytes_consumed = 0;

	pos = pos1;
	endpos = pos1+len;

	while(1) {
		if(unc_pixels->max_len>0 && unc_pixels->len>=unc_pixels->max_len) {
			break; // Decompressed the requested amount of dst data.
		}

		if(pos>=endpos) {
			break; // Reached the end of source data
		}
		b = dbuf_getbyte(f, pos++);

		if(b>128) { // A compressed run
			count = 257 - (de_int64)b;
			b2 = dbuf_getbyte(f, pos++);
			dbuf_write_run(unc_pixels, b2, count);
		}
		else if(b<128) { // An uncompressed run
			count = 1 + (de_int64)b;
			dbuf_copy(f, pos, count, unc_pixels);
			pos += count;
		}
		// Else b==128. No-op.
		// TODO: Some (but not most) ILBM specs say that code 128 is used to
		// mark the end of compressed data, so maybe there should be options to
		// tell us what to do when code 128 is encountered.
	}

	if(cmpr_bytes_consumed) *cmpr_bytes_consumed = pos - pos1;
	return 1;
}
