// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// MAKIchan graphics
//  Supported: .MAG, .MKI
//  TODO: .MAX

#include <deark-config.h>
#include <deark-modules.h>

typedef struct localctx_struct {
	de_int64 width, height;
	de_int64 header_pos;
	de_int64 flag_a_offset;
	de_int64 flag_b_offset;
	de_int64 flag_b_size;
	de_int64 pixels_offset;
	de_int64 pixels_size;
	de_int64 num_colors;
	de_int64 bits_per_pixel;
	de_int64 rowspan;
	de_int64 width_adj, height_adj;

	de_byte aspect_ratio_flag;
	int is_max;
	int is_mki;
	int is_mki_b;
	dbuf *virtual_screen;
	dbuf *unc_pixels;
	de_uint32 pal[256];
} lctx;

static de_int64 de_int_round_up(de_int64 n, de_int64 m)
{
	return ((n+(m-1))/m)*m;
}

static void read_palette(deark *c, lctx *d, de_int64 pos)
{
	de_int64 k;
	de_byte cr, cg, cb;

	de_dbg(c, "palette at %d\n", (int)pos);
	de_dbg_indent(c, 1);

	for(k=0; k<d->num_colors; k++) {
		cg = de_getbyte(pos+3*k);
		cr = de_getbyte(pos+3*k+1);
		cb = de_getbyte(pos+3*k+2);
		d->pal[k] = DE_MAKE_RGB(cr,cg,cb);
		de_dbg_pal_entry(c, k, d->pal[k]);
	}

	de_dbg_indent(c, -1);
}

static int read_mki_header(deark *c, lctx *d)
{
	de_int64 xoffset, yoffset;
	de_int64 width_raw, height_raw;
	de_int64 pos;
	de_int64 flag_a_size;
	de_int64 pix_data_a_size;
	de_int64 pix_data_b_size;
	de_int64 expected_file_size;
	unsigned int extension_flags;
	int retval = 0;

	de_dbg(c, "MKI header at %d\n", (int)d->header_pos);
	de_dbg_indent(c, 1);

	pos = d->header_pos;

	d->flag_b_size = de_getui16be(pos+0);
	pix_data_a_size = de_getui16be(pos+2);
	pix_data_b_size = de_getui16be(pos+4);
	d->pixels_size = pix_data_a_size + pix_data_b_size;

	extension_flags = (unsigned int)de_getui16be(pos+6);
	de_dbg(c, "extension flags: 0x%04x\n", extension_flags);
	de_dbg_indent(c, 1);
	d->aspect_ratio_flag = extension_flags&0x0001;
	if(extension_flags&0x0002) {
		d->num_colors = 8;
	}
	else {
		d->num_colors = 16;
		d->bits_per_pixel = 4;
	}
	de_dbg(c, "number of colors: %d\n", (int)d->num_colors);
	de_dbg_indent(c, -1);

	xoffset = de_getui16be(pos+8);
	yoffset = de_getui16be(pos+10);
	de_dbg(c, "image offset: (%d,%d)\n", (int)xoffset, (int)yoffset);

	width_raw = de_getui16be(pos+12);
	d->width = width_raw - xoffset;
	height_raw = de_getui16be(pos+14);
	d->height = height_raw - yoffset;
	de_dbg(c, "dimensions: %dx%d\n", (int)d->width, (int)d->height);
	if(d->width%64 != 0) {
		de_warn(c, "Width is not a multiple of 64. This image may not be handled correctly.\n");
	}
	d->width_adj = de_int_round_up(d->width, 64);
	if(d->width%64 != 0) {
		de_warn(c, "Height is not a multiple of 4. This image may not be handled correctly.\n");
	}
	d->height_adj = de_int_round_up(d->height, 4);

	d->flag_a_offset = pos + 16 + 48;
	// The documentation seems to say that flag A is *always* 1000 bytes, regardless of
	// how many bytes would actually be needed.
	// This would imply that a MAKI image can't have more than 256000 pixels.
	flag_a_size = 1000;
	//flag_a_size = (d->width_adj*d->height_adj)/256;

	d->flag_b_offset = d->flag_a_offset + flag_a_size;
	d->pixels_offset = d->flag_b_offset + d->flag_b_size;
	expected_file_size = d->pixels_offset + d->pixels_size;
	de_dbg(c, "flag A offset=%d, size=%d\n", (int)d->flag_a_offset, (int)flag_a_size);
	de_dbg(c, "flag B calculated_offset=%d, size=%d\n", (int)d->flag_b_offset, (int)d->flag_b_size);
	de_dbg(c, "pix data size_A=%d, size_B=%d\n", (int)pix_data_a_size, (int)pix_data_b_size);
	de_dbg(c, "pix data calculated_offset=%d, calculated_size=%d\n", (int)d->pixels_offset, (int)d->pixels_size);
	de_dbg(c, "calculated file size: %d\n", (int)expected_file_size);

	if(d->bits_per_pixel!=4 && d->bits_per_pixel!=8) {
		de_err(c, "Unsupported or unknown bits/pixel\n");
		goto done;
	}

	retval = 1;

done:
	de_dbg_indent(c, -1);
	return retval;
}

static void mki_decompress_virtual_screen(deark *c, lctx *d)
{
	de_int64 i, j;
	de_int64 a_pos, b_pos;
	de_int64 vs_rowspan;
	de_int64 k;
	de_byte tmpn[4];
	de_byte v;
	de_byte a_byte = 0x00;
	int a_bitnum;

	vs_rowspan = d->width_adj/16;
	a_pos = d->flag_a_offset;
	a_bitnum = -1;
	b_pos = d->flag_b_offset;
	d->virtual_screen = dbuf_create_membuf(c, vs_rowspan*d->height_adj, 1);

	for(j=0; j<d->height_adj/4; j++) {
		for(i=0; i<d->width_adj/8; i++) {
			de_byte flag_a_bit;

			// Read next flag A bit
			if(a_bitnum<0) {
				a_byte = de_getbyte(a_pos++);
				a_bitnum = 7;
			}
			flag_a_bit = a_byte & (1 << a_bitnum--);

			if(!flag_a_bit)
				continue;

			// Read the next two bytes from flag B, and split them into 4 nibbles.
			tmpn[0] = de_getbyte(b_pos++);
			tmpn[2] = de_getbyte(b_pos++);
			tmpn[1] = tmpn[0]&0x0f;
			tmpn[3] = tmpn[2]&0x0f;
			tmpn[0] >>= 4;
			tmpn[2] >>= 4;

			for(k=0; k<4; k++) {
				de_int64 vs_pos;

				vs_pos = (4*j+k)*vs_rowspan + i/2;
				if(i%2==0) {
					v = tmpn[k]<<4;
				}
				else {
					v = dbuf_getbyte(d->virtual_screen, vs_pos) | tmpn[k];
				}
				dbuf_writebyte_at(d->virtual_screen, vs_pos, v);
			}
		}
	}
}

static void mki_decompress_pixels(deark *c, lctx *d)
{
	de_int64 i, j;
	de_int64 p_pos;
	de_int64 delta_y;
	de_int64 vs_pos;
	int vs_bitnum;
	de_byte vs_byte = 0x00;

	d->rowspan = d->width_adj/2;
	vs_pos = 0;
	vs_bitnum = -1;
	p_pos = d->pixels_offset;
	delta_y = d->is_mki_b ? 4 : 2;
	d->unc_pixels = dbuf_create_membuf(c, d->rowspan*d->height_adj, 1);

	for(j=0; j<d->height; j++) {
		for(i=0; i<d->rowspan; i++) {
			de_byte vs_bit;
			de_byte v;

			// Read the next virtual-screen bit
			if(vs_bitnum<0) {
				vs_byte = dbuf_getbyte(d->virtual_screen, vs_pos++);
				vs_bitnum = 7;
			}
			vs_bit = vs_byte & (1 << vs_bitnum--);

			if(vs_bit) {
				v = de_getbyte(p_pos++);
			}
			else {
				v = 0x00;
			}

			if(j>=delta_y) {
				v ^= dbuf_getbyte(d->unc_pixels, (j-delta_y)*d->rowspan + i);
			}
			dbuf_writebyte(d->unc_pixels, v);
		}
	}
}

static int read_mag_header(deark *c, lctx *d)
{
	de_int64 xoffset, yoffset;
	de_int64 width_raw, height_raw;
	de_int64 pos;
	de_byte model_code;
	de_byte model_flags;
	de_byte screen_mode;
	de_byte colors_code;
	int retval = 0;

	de_dbg(c, "header at %d\n", (int)d->header_pos);
	de_dbg_indent(c, 1);

	pos = d->header_pos;

	model_code = de_getbyte(pos+1);
	model_flags = de_getbyte(pos+2);
	de_dbg(c, "model code: 0x%02x, flags: 0x%02x\n",
		(unsigned int)model_code, (unsigned int)model_flags);
	if(model_code==0x03 && ((model_flags&0x44)==0x44)) { // Just a guess
		de_warn(c, "This looks like MAX format, which is not correctly supported.\n");
		d->is_max = 1;
	}

	screen_mode = de_getbyte(pos+3);
	de_dbg(c, "screen mode: %d\n", (int)screen_mode);
	de_dbg_indent(c, 1);
	d->aspect_ratio_flag = screen_mode&0x01;
	colors_code = screen_mode&0x82;
	if(colors_code==0x00) {
		d->num_colors = 16;
		d->bits_per_pixel = 4;
	}
	else if(colors_code==0x80) {
		d->num_colors = 256;
		d->bits_per_pixel = 8;
	}
	else if(colors_code==0x02) {
		d->num_colors = 8;
		// TODO: Support 8 color images
	}
	de_dbg(c, "number of colors: %d\n", (int)d->num_colors);
	de_dbg_indent(c, -1);

	xoffset = de_getui16le(pos+4);
	yoffset = de_getui16le(pos+6);
	de_dbg(c, "image offset: (%d,%d)\n", (int)xoffset, (int)yoffset);

	width_raw = de_getui16le(pos+8);
	height_raw = de_getui16le(pos+10);
	d->width = width_raw - xoffset + 1;
	d->height = height_raw - yoffset + 1;
	de_dbg(c, "dimensions: %dx%d\n", (int)d->width, (int)d->height);

	d->flag_a_offset = de_getui32le(pos+12);
	d->flag_a_offset += d->header_pos;
	de_dbg(c, "flag A offset: %d\n", (int)d->flag_a_offset);

	d->flag_b_offset = de_getui32le(pos+16);
	d->flag_b_offset += d->header_pos;
	d->flag_b_size = de_getui32le(pos+20);
	de_dbg(c, "flag B offset: %d, size=%d\n", (int)d->flag_b_offset, (int)d->flag_b_size);

	d->pixels_offset = de_getui32le(pos+24);
	d->pixels_offset += d->header_pos;
	d->pixels_size = de_getui32le(pos+28);
	de_dbg(c, "pixels offset: %d, size=%d\n", (int)d->pixels_offset, (int)d->pixels_size);

	if(d->bits_per_pixel!=4 && d->bits_per_pixel!=8) {
		de_err(c, "Unsupported or unknown bits/pixel\n");
		goto done;
	}

	retval = 1;
done:
	de_dbg_indent(c, -1);
	return retval;
}

static int do_mag_decompress(deark *c, lctx *d)
{
	static const de_byte delta_x[16] = { 0,1,2,4,0,1,0,1,2,0,1,2,0,1,2, 0 };
	static const de_byte delta_y[16] = { 0,0,0,0,1,1,2,2,2,4,4,4,8,8,8,16 };
	de_int64 x, y;
	de_int64 a_pos, b_pos;
	de_int64 p_pos;
	int a_bitnum; // Index of next bit to read. -1 = no more bits in a_byte.
	de_byte a_byte = 0x00;
	de_byte b_byte;
	int k;
	de_int64 dpos;
	de_byte *action_byte_buf = NULL;
	de_byte wordbuf[2];

	de_dbg(c, "decompressing pixels\n");

	// Presumably, due to the compression scheme, every row must have a
	// multiple of 4 bytes.
	d->rowspan = ((d->width * d->bits_per_pixel + 31)/32)*4;

	d->unc_pixels = dbuf_create_membuf(c, d->rowspan * d->height, 1);

	a_pos = d->flag_a_offset;
	a_bitnum = -1;
	b_pos = d->flag_b_offset;
	p_pos = d->pixels_offset;

	action_byte_buf = de_malloc(c, d->rowspan/4);

	for(y=0; y<d->height; y++) {
		for(x=0; x<d->rowspan/4; x++) {
			de_byte action_byte;
			de_byte flag_a_bit;

			// Read next flag A bit
			if(a_bitnum<0) {
				a_byte = de_getbyte(a_pos++);
				a_bitnum = 7;
			}
			flag_a_bit = a_byte & (1 << a_bitnum--);

			if(flag_a_bit) {
				// If flag_a_bit is unset, re-use the action byte from the
				// previous row.
				// If flag bit A is set, the new action byte is the one from the
				// previous row XORed with the next B byte (don't ask me why).
				b_byte = de_getbyte(b_pos++);
				action_byte_buf[x] ^= b_byte;
			}

			action_byte = action_byte_buf[x];

			// Produce 4 uncompressed bytes, 2 for each nibble in the
			// action byte.
			for(k=0; k<2; k++) {
				unsigned int dcode;

				if(k==0)
					dcode = (unsigned int)((action_byte&0xf0)>>4);
				else
					dcode = (unsigned int)(action_byte&0x0f);

				if(dcode==0) {
					// An "uncompressed" data word. Read it from the source file.
					de_read(wordbuf, p_pos, 2);
					p_pos += 2;
				}
				else {
					// Copy the data word from an earlier location in the image.
					dpos = d->unc_pixels->len -
						d->rowspan*(de_int64)delta_y[dcode] -
						2*(de_int64)delta_x[dcode];
					dbuf_read(d->unc_pixels, wordbuf, dpos, 2);
				}
				dbuf_write(d->unc_pixels, wordbuf, 2);
			}
		}
	}

	de_free(c, action_byte_buf);
	return 1;
}

static void do_create_image(deark *c, lctx *d)
{
	struct deark_bitmap *img = NULL;

	img = de_bitmap_create(c, d->width, d->height, 3);

	if(d->aspect_ratio_flag) {
		img->density_code = DE_DENSITY_UNK_UNITS;
		img->xdens = 2.0;
		img->ydens = 1.0;
	}

	de_convert_image_paletted(d->unc_pixels, 0,
		d->bits_per_pixel, d->rowspan, d->pal, img, 0);

	de_bitmap_write_to_file(img, NULL);
	de_bitmap_destroy(img);
}

// Sets d->header_pos
static int find_mag_header(deark *c, lctx *d)
{
	de_int64 pos_1a = 0;
	int ret;

	// Find the first 0x1a byte.
	ret = dbuf_search_byte(c->infile, '\x1a', 0, c->infile->len, &pos_1a);
	if(ret) {
		// Find the first 0x00 byte after the first 0x1a byte.
		// TODO: Is this the correct algorithm, or should we just assume the
		// header starts immediately after the first 0x1a byte?
		ret = dbuf_search_byte(c->infile, '\0', pos_1a+1, c->infile->len-pos_1a-1, &d->header_pos);
		de_dbg(c, "header found at %d\n", (int)d->header_pos);
		return 1;
	}

	de_err(c, "Failed to find header. This is probably not a MAKIchan file.\n");
	return 0;
}

static void do_mag(deark *c, lctx *d)
{
	if(!find_mag_header(c, d)) goto done;
	if(!read_mag_header(c, d)) goto done;
	read_palette(c, d, d->header_pos+32);
	if(!de_good_image_dimensions(c, d->width, d->height)) goto done;
	if(!do_mag_decompress(c, d)) goto done;
	do_create_image(c, d);
done:
	;
}

static void do_mki(deark *c, lctx *d)
{
	d->header_pos = 32;
	if(!read_mki_header(c, d)) goto done;
	read_palette(c, d, d->header_pos+16);
	if(!de_good_image_dimensions(c, d->width, d->height)) goto done;
	mki_decompress_virtual_screen(c, d);
	mki_decompress_pixels(c, d);
	do_create_image(c, d);
done:
	;
}

static void de_run_makichan(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));

	if(!dbuf_memcmp(c->infile, 0, "MAKI01", 6)) {
		d->is_mki = 1;
		if(de_getbyte(6)=='B') {
			d->is_mki_b = 1;
		}
	}

	if(d->is_mki) {
		do_mki(c, d);
	}
	else {
		do_mag(c, d);
	}

	dbuf_close(d->unc_pixels);
	dbuf_close(d->virtual_screen);
	de_free(c, d);
}

static int de_identify_makichan(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "MAKI0", 5))
		return 100;
	return 0;
}

void de_module_makichan(deark *c, struct deark_module_info *mi)
{
	mi->id = "makichan";
	mi->desc = "MAKIchan graphics";
	mi->run_fn = de_run_makichan;
	mi->identify_fn = de_identify_makichan;
}