// This file is part of Deark.
// Copyright (C) 2018 Jason Summers
// See the file COPYING for terms of use.

// Microsoft Windows Write (.wri) format

#include <deark-config.h>
#include <deark-private.h>
DE_DECLARE_MODULE(de_module_wri);

struct text_styles_struct {
	u8 tab_style;
};

struct para_info {
	i64 thisparapos, thisparalen;
	i64 bfprop_offset; // file-level offset
	u8 papflags;
	u8 justification;

	int in_para;
	int xpos; // Current length of this line in the source code
	int has_content; // Have we emitted a non-space char in this paragraph?
	int space_count;

	int in_span;
	struct text_styles_struct text_styles_wanted; // Styles for the next char to be emitted
	struct text_styles_struct text_styles_current; // Effective current styles
};

typedef struct localctx_struct {
	int extract_text;
	int input_encoding;
	int ddbhack;
	i64 fcMac;
	i64 pnChar;
	i64 pnChar_offs;
	i64 pnPara;
	i64 pnPara_offs;
	i64 pnPara_npages;
	i64 pnFntb, pnSep, pnSetb, pnPgtb, pnFfntb;
	i64 pnMac;
	dbuf *html_outf;
} lctx;

static void do_emit_raw_sz(deark *c, lctx *d, struct para_info *pinfo, const char *sz);

static void default_text_styles(struct text_styles_struct *ts)
{
	de_zeromem(ts, sizeof(struct text_styles_struct));
}

static int text_styles_differ(const struct text_styles_struct *ts1,
	const struct text_styles_struct *ts2)
{
	if(ts1->tab_style != ts2->tab_style) return 1;
	return 0;
}

static int do_header(deark *c, lctx *d, i64 pos)
{
	de_dbg(c, "header at %d", (int)pos);
	de_dbg_indent(c, 1);

	d->fcMac = de_getu32le(pos+7*2);
	de_dbg(c, "fcMac: %d", (int)d->fcMac);
	d->pnChar = (d->fcMac + 127) / 128;
	d->pnChar_offs = d->pnChar * 128;
	de_dbg(c, "pnChar: page %d (offset %d)", (int)d->pnChar, (int)d->pnChar_offs);

	d->pnPara = de_getu16le(pos+9*2);
	d->pnPara_offs = d->pnPara * 128;
	de_dbg(c, "pnPara: page %d (offset %d)", (int)d->pnPara, (int)d->pnPara_offs);

	d->pnFntb = de_getu16le(pos+10*2);
	de_dbg(c, "pnFntb: page %d", (int)d->pnFntb);

	d->pnSep = de_getu16le(pos+11*2);
	de_dbg(c, "pnSep: page %d", (int)d->pnSep);

	d->pnSetb = de_getu16le(pos+12*2);
	de_dbg(c, "pnSetb: page %d", (int)d->pnSetb);

	d->pnPgtb = de_getu16le(pos+13*2);
	de_dbg(c, "pnPgtb: page %d", (int)d->pnPgtb);

	d->pnFfntb = de_getu16le(pos+14*2);
	de_dbg(c, "pnFfntb: page %d", (int)d->pnFfntb);

	d->pnMac = de_getu16le(pos+48*2);
	de_dbg(c, "pnMac: %d pages", (int)d->pnMac);

	d->pnPara_npages = d->pnFntb - d->pnPara;

	de_dbg_indent(c, -1);
	return 1;
}

static void do_picture_metafile(deark *c, lctx *d, struct para_info *pinfo)
{
	i64 pos = pinfo->thisparapos;
	i64 cbHeader, cbSize;

	cbHeader = de_getu16le(pos+30);
	de_dbg(c, "cbHeader: %d", (int)cbHeader);

	cbSize = de_getu32le(pos+32);
	de_dbg(c, "cbSize: %d", (int)cbSize);

	if(cbHeader+cbSize <= pinfo->thisparalen) {
		dbuf_create_file_from_slice(c->infile, pos+cbHeader, cbSize, "wmf", NULL, 0);
	}
}

static void do_picture_bitmap(deark *c, lctx *d, struct para_info *pinfo)
{
	i64 pos = pinfo->thisparapos;
	i64 cbHeader, cbSize;
	i64 bmWidth, bmHeight;
	i64 bmBitsPixel;
	i64 rowspan;

	bmWidth = de_getu16le(pos+16+2);
	bmHeight = de_getu16le(pos+16+4);
	de_dbg_dimensions(c, bmWidth, bmHeight);
	bmBitsPixel = (i64)de_getbyte(pos+16+9);
	de_dbg(c, "bmBitsPixel: %d", (int)bmBitsPixel);

	rowspan = de_getu16le(pos+16+6);
	de_dbg(c, "bytes/row: %d", (int)rowspan);

	cbHeader = de_getu16le(pos+30);
	de_dbg(c, "cbHeader: %d", (int)cbHeader);

	cbSize = de_getu32le(pos+32);
	de_dbg(c, "cbSize: %d", (int)cbSize);

	if(bmBitsPixel!=1) {
		de_err(c, "This type of bitmap is not supported (bmBitsPixel=%d)",
			(int)bmBitsPixel);
		goto done;
	}

	pos += cbHeader;

	de_convert_and_write_image_bilevel(c->infile, pos, bmWidth, bmHeight, rowspan, 0, NULL, 0);

done:
	;
}

static const char *get_objecttype1_name(unsigned int t)
{
	const char *name;
	switch(t) {
	case 1: name="static"; break;
	case 2: name="embedded"; break;
	case 3: name="link"; break;
	default: name="?"; break;
	}
	return name;
}

static const char *get_picture_storage_type_name(unsigned int t)
{
	const char *name;
	switch(t) {
	case 0x88: name="metafile"; break;
	case 0xe3: name="bitmap"; break;
	case 0xe4: name="OLE object"; break;
	default: name="?"; break;
	}
	return name;
}

// TODO: This does not really work, and is disabled by default.
// TODO: Can this be merged with do_picture_bitmap()?
static void do_static_bitmap(deark *c, lctx *d, struct para_info *pinfo, i64 pos1)
{
	i64 dlen;
	i64 pos = pos1;
	i64 bmWidth, bmHeight;
	i64 bmBitsPixel;
	i64 rowspan;

	pos += 8; // ??
	dlen = de_getu32le_p(&pos);
	de_dbg(c, "bitmap size: %d", (int)dlen);

	pos += 2; // bmType
	bmWidth = de_getu16le_p(&pos);
	bmHeight = de_getu16le_p(&pos);
	de_dbg_dimensions(c, bmWidth, bmHeight);

	rowspan = de_getu16le_p(&pos);
	de_dbg(c, "bytes/row: %d", (int)rowspan);

	pos++; // bmPlanes

	bmBitsPixel = (i64)de_getbyte_p(&pos);
	de_dbg(c, "bmBitsPixel: %d", (int)bmBitsPixel);

	pos += 4; // bmBits

	if(bmBitsPixel!=1) {
		de_err(c, "This type of bitmap is not supported (bmBitsPixel=%d)",
			(int)bmBitsPixel);
		goto done;
	}

	rowspan = (dlen-14)/bmHeight;
	bmWidth = rowspan*8;
	de_convert_and_write_image_bilevel(c->infile, pos, bmWidth, bmHeight, rowspan, 0, NULL, 0);

done:
	;
}

static int do_picture_ole_static_rendition(deark *c, lctx *d, struct para_info *pinfo,
	int rendition_idx, i64 pos1, i64 *bytes_consumed)
{
	i64 pos = pos1;
	i64 stringlen;
	struct de_stringreaderdata *srd_typename = NULL;

	pos += 4; // 0x00000501
	pos += 4; // "type" (probably already read by caller)

	stringlen = de_getu32le_p(&pos);
	srd_typename = dbuf_read_string(c->infile, pos, stringlen, 260, DE_CONVFLAG_STOP_AT_NUL,
		DE_ENCODING_ASCII);
	de_dbg(c, "typename: \"%s\"", ucstring_getpsz(srd_typename->str));
	pos += stringlen;

	if(!de_strcmp(srd_typename->sz, "DIB")) {
		pos += 12;
		de_dbg_indent(c, 1);
		de_run_module_by_id_on_slice(c, "dib", NULL, c->infile, pos,
			pinfo->thisparapos+pinfo->thisparalen-pos);
		de_dbg_indent(c, -1);
	}
	else if(!de_strcmp(srd_typename->sz, "METAFILEPICT")) {
		i64 dlen;
		pos += 8; // ??
		dlen = de_getu32le_p(&pos);
		de_dbg(c, "metafile size: %d", (int)dlen); // Includes "mfp", apparently
		pos += 8; // "mfp" struct
		dbuf_create_file_from_slice(c->infile, pos, dlen-8, "wmf", NULL, 0);
	}
	else if(d->ddbhack && !de_strcmp(srd_typename->sz, "BITMAP")) {
		do_static_bitmap(c, d, pinfo, pos);
	}
	else {
		de_warn(c, "Static OLE picture type \"%s\" is not supported",
			ucstring_getpsz(srd_typename->str));
	}

	de_destroy_stringreaderdata(c, srd_typename);
	return 0;
}

// pos1 points to the ole_id field (should be 0x00000501).
// Caller must have looked ahead to check the type.
static int do_picture_ole_embedded_rendition(deark *c, lctx *d, struct para_info *pinfo,
	int rendition_idx, i64 pos1, i64 *bytes_consumed)
{
	i64 pos = pos1;
	i64 stringlen;
	i64 data_len;
	u8 buf[2];
	struct de_stringreaderdata *srd_typename = NULL;
	struct de_stringreaderdata *srd_filename = NULL;
	struct de_stringreaderdata *srd_params = NULL;

	pos += 4; // 0x00000501
	pos += 4; // "type" (probably already read by caller)

	stringlen = de_getu32le_p(&pos);
	srd_typename = dbuf_read_string(c->infile, pos, stringlen, 260, DE_CONVFLAG_STOP_AT_NUL,
		DE_ENCODING_ASCII);
	de_dbg(c, "typename: \"%s\"", ucstring_getpsz(srd_typename->str));
	pos += stringlen;

	stringlen = de_getu32le_p(&pos);
	srd_filename = dbuf_read_string(c->infile, pos, stringlen, 260, DE_CONVFLAG_STOP_AT_NUL,
		DE_ENCODING_ASCII);
	de_dbg(c, "filename: \"%s\"", ucstring_getpsz(srd_filename->str));
	pos += stringlen;

	stringlen = de_getu32le_p(&pos);
	srd_params = dbuf_read_string(c->infile, pos, stringlen, 260, DE_CONVFLAG_STOP_AT_NUL,
		DE_ENCODING_ASCII);
	de_dbg(c, "params: \"%s\"", ucstring_getpsz(srd_params->str));
	pos += stringlen;

	data_len = de_getu32le_p(&pos);
	de_dbg(c, "embedded ole rendition data: pos=%d, len=%d", (int)pos, (int)data_len);

	// TODO: I don't know if it's better to sniff the data, or rely on the typename.
	de_read(buf, pos, 2);
	if(buf[0]=='B' && buf[1]=='M') {
		// TODO: Detect true length of data
		dbuf_create_file_from_slice(c->infile, pos, data_len, "bmp", NULL, 0);
	}
	else {
		de_warn(c, "Unknown/unsupported type of OLE object (\"%s\") at %d",
			ucstring_getpsz(srd_typename->str), (int)pos1);
	}

	pos += data_len;
	*bytes_consumed = pos - pos1;

	de_destroy_stringreaderdata(c, srd_typename);
	de_destroy_stringreaderdata(c, srd_filename);
	de_destroy_stringreaderdata(c, srd_params);
	return 1;
}

// pos1 points to the ole_id field (should be 0x00000501).
// Returns nonzero if there may be additional renditions.
static int do_picture_ole_rendition(deark *c, lctx *d, struct para_info *pinfo,
	unsigned int objectType,
	int rendition_idx, i64 pos1, i64 *bytes_consumed)
{
	unsigned int ole_id;
	unsigned int objectType2;
	int retval = 0;

	de_dbg(c, "OLE rendition[%d] at %d", rendition_idx, (int)pos1);
	de_dbg_indent(c, 1);

	ole_id = (unsigned int)de_getu32le(pos1);
	de_dbg(c, "ole id: 0x%08x", ole_id);
	if(ole_id!=0x00000501U) {
		de_err(c, "Unexpected ole_id: 0x%08x", ole_id);
		goto done;
	}

	objectType2 = (unsigned int)de_getu32le(pos1+4);
	de_dbg(c, "type: %u", objectType2);

	if(objectType==1) {
		if(objectType2==3) {
			do_picture_ole_static_rendition(c, d, pinfo, rendition_idx, pos1, bytes_consumed);
		}
	}
	else if(objectType==2) {
		if(objectType2==0) {
			goto done;
		}
		else if(objectType2==2) {
			do_picture_ole_embedded_rendition(c, d, pinfo, rendition_idx, pos1, bytes_consumed);
			retval = 1;
		}
		else if(objectType2==5) { // replacement
			do_picture_ole_static_rendition(c, d, pinfo, rendition_idx, pos1, bytes_consumed);
		}
	}

done:
	de_dbg_indent(c, -1);
	return retval;
}

static void do_picture_ole(deark *c, lctx *d, struct para_info *pinfo)
{
	unsigned int objectType;
	i64 cbHeader, dwDataSize;
	i64 pos = pinfo->thisparapos;
	i64 nbytes_left;
	i64 bytes_consumed = 0;
	int rendition_idx = 0;

	objectType = (unsigned int)de_getu16le(pos+6);
	de_dbg(c, "objectType: %u (%s)", objectType, get_objecttype1_name(objectType));

	dwDataSize = de_getu32le(pos+16);
	de_dbg(c, "dwDataSize: %d", (int)dwDataSize);

	cbHeader = de_getu16le(pos+30);
	de_dbg(c, "cbHeader: %d", (int)cbHeader);

	pos += cbHeader;

	// An "embedded" OLE object contains a sequence of entities that I'll call
	// renditions. The last entity is just an end-of-sequence marker.
	// I'm not sure if there can be more than two renditions (one embedded, and
	// one static "replacement"), or if the order matters.

	while(1) {
		int ret;
		nbytes_left = pinfo->thisparapos + pinfo->thisparalen - pos;
		if(nbytes_left<8) break;

		bytes_consumed = 0;
		ret = do_picture_ole_rendition(c, d, pinfo, objectType, rendition_idx, pos,
			&bytes_consumed);
		if(!ret || bytes_consumed==0) break;
		pos += bytes_consumed;
		rendition_idx++;
	}
}

static void do_picture(deark *c, lctx *d, struct para_info *pinfo)
{
	unsigned int mm;
	i64 pos = pinfo->thisparapos;

	if(d->html_outf) {
		do_emit_raw_sz(c, d, pinfo, "<p class=r>picture</p>\n");
	}

	if(pinfo->thisparalen<2) goto done;
	mm = (unsigned int)de_getu16le(pos);
	de_dbg(c, "picture storage type: 0x%04x (%s)", mm,
		get_picture_storage_type_name(mm));

	switch(mm) {
	case 0x88:
		do_picture_metafile(c, d, pinfo);
		break;
	case 0xe3:
		do_picture_bitmap(c, d, pinfo);
		break;
	case 0xe4:
		do_picture_ole(c, d, pinfo);
		break;
	default:
		de_err(c, "Picture storage type 0x%04x not supported", mm);
	}

done:
	;
}

static void ensure_in_para(deark *c, lctx *d, struct para_info *pinfo)
{
	if(pinfo->in_para) return;
	do_emit_raw_sz(c, d, pinfo, "<p");
	switch(pinfo->justification) {
	case 1: do_emit_raw_sz(c, d, pinfo, " class=tc"); break;
	case 2: do_emit_raw_sz(c, d, pinfo, " class=tr"); break;
	case 3: do_emit_raw_sz(c, d, pinfo, " class=tj"); break;
	}
	do_emit_raw_sz(c, d, pinfo, ">");
	pinfo->in_para = 1;
}

// Emit a data codepoint, inside a paragraph.
static void do_emit_codepoint(deark *c, lctx *d, struct para_info *pinfo, i32 outcp)
{
	int styles_changed;

	if(!pinfo->in_para) {
		ensure_in_para(c, d, pinfo);
	}

	styles_changed = text_styles_differ(&pinfo->text_styles_current, &pinfo->text_styles_wanted);

	if(pinfo->in_span && styles_changed) {
		do_emit_raw_sz(c, d, pinfo, "</span>");
		pinfo->in_span = 0;
	}
	if(styles_changed) {
		if(pinfo->text_styles_wanted.tab_style) {
			do_emit_raw_sz(c, d, pinfo, "<span class=c>");
			pinfo->in_span = 1;
		}
		pinfo->text_styles_current = pinfo->text_styles_wanted; // struct copy
	}

	de_write_codepoint_to_html(c, d->html_outf, outcp);

	// FIXME: We'd like to know how many characters (not bytes) were written,
	// but we don't currently have a good way to do that in the case where the
	// codepoint was written as an HTML entity.
	pinfo->xpos++;

	if(outcp!=32) {
		pinfo->has_content = 1;
	}
}

// Emit a raw string. Does not force a paragraph to be open.
// Updates pinfo->xpos (assumes 1 byte per char).
// For xpos, handles the case where sz ends with a newline, but does not
// handle internal newlines.
static void do_emit_raw_sz(deark *c, lctx *d, struct para_info *pinfo, const char *sz)
{
	size_t sz_len = de_strlen(sz);
	if(sz_len<1) return;
	dbuf_write(d->html_outf, (const u8*)sz, (i64)sz_len);
	if(sz[sz_len-1]=='\n') {
		pinfo->xpos = 0;
	}
	else {
		pinfo->xpos += (int)sz_len;
	}
}

static void end_para(deark *c, lctx *d, struct para_info *pinfo)
{
	if(!pinfo->in_para) return;

	if(pinfo->in_span) {
		do_emit_raw_sz(c, d, pinfo, "</span>");
		pinfo->in_span = 0;
	}

	if(!pinfo->has_content) {
		// No empty paragraphs allowed. HTML will collapse them, but Write does not.
		do_emit_codepoint(c, d, pinfo, 0xa0);
	}
	do_emit_raw_sz(c, d, pinfo, "</p>\n");
	pinfo->in_para = 0;
	default_text_styles(&pinfo->text_styles_current);
}

static void do_text_paragraph(deark *c, lctx *d, struct para_info *pinfo)
{
	i64 i, k;

	if(!d->html_outf) return;

	if((pinfo->papflags & 0x06)!=0) {
		// TODO: Decode headers and footers somehow.
		do_emit_raw_sz(c, d, pinfo, "<p class=r>");
		do_emit_raw_sz(c, d, pinfo, (pinfo->papflags&0x01)?"footer":"header");
		do_emit_raw_sz(c, d, pinfo, " definition</p>\n");
		return;
	}

	pinfo->in_para = 0;
	pinfo->xpos = 0;
	pinfo->space_count = 0;
	pinfo->has_content = 0;
	pinfo->in_span = 0;
	default_text_styles(&pinfo->text_styles_wanted);
	default_text_styles(&pinfo->text_styles_current);

	for(i=0; i<pinfo->thisparalen; i++) {
		u8 incp;

		incp = de_getbyte(pinfo->thisparapos+i);
		if(incp==0x0d && i<pinfo->thisparalen-1) {
			if(de_getbyte(pinfo->thisparapos+i+1)==0x0a) {
				// Found CR-LF combo
				i++;
				ensure_in_para(c, d, pinfo);
				end_para(c, d, pinfo);
				continue;
			}
		}

		if(incp!=32 && pinfo->space_count>0) {
			int nonbreaking_count, breaking_count;

			if(!pinfo->in_para && pinfo->space_count==1) {
				// If the paragraph starts with a single space, make it nonbreaking.
				nonbreaking_count = 1;
				breaking_count = 0;
			}
			else {
				// Else make all spaces but the last one nonbreaking
				nonbreaking_count = pinfo->space_count-1;
				breaking_count = 1;
			}

			ensure_in_para(c, d, pinfo);

			for(k=0; k<nonbreaking_count; k++) {
				do_emit_codepoint(c, d, pinfo, 0xa0);
			}

			if(breaking_count>0) {
				if(pinfo->xpos>70) {
					// We don't do proper word wrapping of the HTML source, but
					// maybe this is better than nothing.
					do_emit_raw_sz(c, d, pinfo, "\n");
				}
				else {
					do_emit_codepoint(c, d, pinfo, 32);
				}
			}

			pinfo->space_count=0;
		}

		if(incp>=33) {
			i32 outcp;
			outcp = de_char_to_unicode(c, (i32)incp, d->input_encoding);
			do_emit_codepoint(c, d, pinfo, outcp);
		}
		else {
			switch(incp) {
			case 9: // tab
				pinfo->text_styles_wanted.tab_style = 1;
				do_emit_codepoint(c, d, pinfo, 0x2192);
				pinfo->text_styles_wanted.tab_style = 0;
				break;
			case 10:
			case 11:
				ensure_in_para(c, d, pinfo);
				do_emit_raw_sz(c, d, pinfo, "<br>\n");
				pinfo->has_content = 1;
				break;
			case 12: // page break
				end_para(c, d, pinfo);
				do_emit_raw_sz(c, d, pinfo, "<hr>\n");
				break;
			case 31:
				break;
			case 32:
				pinfo->space_count++;
				break;
			default:
				do_emit_codepoint(c, d, pinfo, 0xfffd);
			}
		}
	}

	end_para(c, d, pinfo);
}

static void do_paragraph(deark *c, lctx *d, struct para_info *pinfo)
{
	if(pinfo->papflags&0x10) {
		de_dbg(c, "picture at %d, len=%d", (int)pinfo->thisparapos,
			(int)pinfo->thisparalen);
		de_dbg_indent(c, 1);
		do_picture(c, d, pinfo);
		de_dbg_indent(c, -1);
	}
	else {
		de_dbg(c, "text paragraph at %d, len=%d", (int)pinfo->thisparapos,
			(int)pinfo->thisparalen);
		do_text_paragraph(c, d, pinfo);
	}
}

static void do_para_fprop(deark *c, lctx *d, struct para_info *pinfo,
	i64 bfprop, u8 is_dup)
{
	i64 fprop_dlen = 0;

	// bfprop is a pointer into the 123 bytes of data starting
	// at pos+4. The maximum sensible value is at most 122.
	if(bfprop<=122) {
		// It appears that the length prefix does not include itself,
		// contrary to what one source says.
		fprop_dlen = (i64)de_getbyte(pinfo->bfprop_offset);
		if(!is_dup) de_dbg(c, "fprop dlen: %d", (int)fprop_dlen);
	}

	if(fprop_dlen>=2) {
		pinfo->justification = de_getbyte(pinfo->bfprop_offset + 1 + 1) & 0x03;
		if(!is_dup && pinfo->justification!=0) {
			de_dbg(c, "justification: %d", (int)pinfo->justification);
		}
	}

	if(fprop_dlen>=17) {
		pinfo->papflags = de_getbyte(pinfo->bfprop_offset + 1 + 16);
		if(!is_dup) {
			de_ucstring *flagstr = ucstring_create(c);
			if(pinfo->papflags&0x06) {
				ucstring_append_flags_item(flagstr, (pinfo->papflags&0x01)?"footer":"header");
				ucstring_append_flags_item(flagstr, (pinfo->papflags&0x08)?"print on first page":
					"do not print on first page");
			}
			if(pinfo->papflags&0x10) ucstring_append_flags_item(flagstr, "picture");
			de_dbg(c, "paragraph flags: 0x%02x (%s)", (unsigned int)pinfo->papflags,
				ucstring_getpsz(flagstr));
			ucstring_destroy(flagstr);
		}
	}
}

static void do_para_info_page(deark *c, lctx *d, i64 pos)
{
	i64 fcFirst;
	i64 cfod;
	i64 i;
	i64 fod_array_startpos;
	i64 prevtextpos;
	u8 fprop_seen[128];

	de_zeromem(fprop_seen, sizeof(fprop_seen));
	de_dbg(c, "paragraph info page at %d", (int)pos);
	de_dbg_indent(c, 1);

	cfod = (i64)de_getbyte(pos+127);
	de_dbg(c, "number of FODs on this page: %d", (int)cfod);

	// There are up to 123 bytes available for the FOD array, and each FOD is
	// 6 bytes. So I assume the maximum possible is 20.
	if(cfod>20) cfod=20;

	fcFirst = de_getu32le(pos);
	de_dbg(c, "fcFirst: %d", (int)fcFirst);

	fod_array_startpos = pos + 4;

	prevtextpos = fcFirst;

	for(i=0; i<cfod; i++) {
		struct para_info *pinfo = NULL;
		i64 fcLim_orig, fcLim_adj;
		i64 bfprop;
		i64 fodpos = fod_array_startpos + 6*i;

		pinfo = de_malloc(c, sizeof(struct para_info));

		de_dbg(c, "FOD[%d] at %d", (int)i, (int)fodpos);
		de_dbg_indent(c, 1);

		fcLim_orig = de_getu32le(fodpos);
		fcLim_adj = fcLim_orig;
		if(fcLim_adj > d->fcMac) fcLim_adj = d->fcMac;
		pinfo->thisparapos = prevtextpos;
		pinfo->thisparalen = fcLim_adj - prevtextpos;
		de_dbg(c, "fcLim: %d (paragraph from %d to %d)", (int)fcLim_orig,
			(int)pinfo->thisparapos, (int)(fcLim_adj-1));
		prevtextpos = fcLim_adj;

		bfprop = de_getu16le(fodpos+4);
		if(bfprop==0xffff) {
			de_dbg(c, "bfprop: %d (none)", (int)bfprop);
		}
		else {
			pinfo->bfprop_offset = fod_array_startpos + bfprop;

			de_dbg(c, "bfprop: %d (+ %d = %d)", (int)bfprop,
				(int)fod_array_startpos, (int)pinfo->bfprop_offset);

			de_dbg_indent(c, 1);
			if(bfprop<128) {
				if(fprop_seen[bfprop]) {
					// An FPROP can be referenced multiple times. Only print the
					// debug info for it once.
					de_dbg(c, "[already decoded FPROP at %d on this paragraph info page]", (int)bfprop);
				}
				do_para_fprop(c, d, pinfo, bfprop, fprop_seen[bfprop]);
				fprop_seen[bfprop] = 1;
			}
			de_dbg_indent(c, -1);
		}

		do_paragraph(c, d, pinfo);

		de_free(c, pinfo);
		pinfo = NULL;
		de_dbg_indent(c, -1);
	}

	de_dbg_indent(c, -1);
}

static void do_para_info(deark *c, lctx *d)
{
	i64 i;

	if(d->pnPara_npages<1) return;
	de_dbg(c, "paragraph info at %d, len=%d page(s)", (int)d->pnPara_offs, (int)d->pnPara_npages);

	de_dbg_indent(c, 1);
	for(i=0; i<d->pnPara_npages; i++) {
		do_para_info_page(c, d, d->pnPara_offs + 128*i);
	}
	de_dbg_indent(c, -1);
}

static void do_html_begin(deark *c, lctx *d)
{
	dbuf *f;
	if(d->html_outf) return;
	d->html_outf = dbuf_create_output_file(c, "html", NULL, 0);
	f = d->html_outf;
	if(c->write_bom && !c->ascii_html) dbuf_write_uchar_as_utf8(f, 0xfeff);
	dbuf_puts(f, "<!DOCTYPE html>\n");
	dbuf_puts(f, "<html>\n");
	dbuf_puts(f, "<head>\n");
	dbuf_printf(f, "<meta charset=\"%s\">\n", c->ascii_html?"US-ASCII":"UTF-8");
	dbuf_puts(f, "<title></title>\n");

	dbuf_puts(f, "<style type=\"text/css\">\n");
	dbuf_puts(f, " body { color: #000; background-color: #fff }\n");
	dbuf_puts(f, " p { margin-top: 0; margin-bottom: 0 }\n");
	dbuf_puts(f, " .c { color: #ccc }\n"); // Visible control characters

	// Replacement object
	dbuf_puts(f, " .r { padding: 0.5ex; color: #800; background-color: #eee;\n");
	dbuf_puts(f, "  font-style: italic; border: 0.34ex dotted #800 }\n");

	dbuf_puts(f, " .tc { text-align: center }\n");
	dbuf_puts(f, " .tr { text-align: right }\n");
	dbuf_puts(f, " .tj { text-align: justify }\n");
	dbuf_puts(f, "</style>\n");

	dbuf_puts(f, "</head>\n");
	dbuf_puts(f, "<body>\n");
}

static void do_html_end(deark *c, lctx *d)
{
	if(!d->html_outf) return;
	dbuf_puts(d->html_outf, "</body>\n</html>\n");
	dbuf_close(d->html_outf);
	d->html_outf = NULL;
}

static void de_run_wri(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;
	i64 pos;

	d = de_malloc(c, sizeof(lctx));

	d->extract_text = 1;
	if(c->input_encoding==DE_ENCODING_UNKNOWN)
		d->input_encoding = DE_ENCODING_WINDOWS1252;
	else
		d->input_encoding = c->input_encoding;

	if(de_get_ext_option(c, "wri:ddbhack")) {
		d->ddbhack = 1;
	}

	pos = 0;
	if(!do_header(c, d, pos)) goto done;
	if(d->extract_text) {
		do_html_begin(c, d);
	}

	do_para_info(c, d);

done:
	do_html_end(c, d);
	de_free(c, d);
}

static int de_identify_wri(deark *c)
{
	u8 buf[6];
	de_read(buf, 0, 6);

	if((buf[0]==0x31 || buf[0]==0x32) &&
		!de_memcmp(&buf[1], "\xbe\x00\x00\x00\xab", 5))
	{
		i64 pnMac;
		pnMac = de_getu16le(48*2);
		if(pnMac==0) return 0; // Apparently MSWord, not Write
		return 100;
	}
	return 0;
}

void de_module_wri(deark *c, struct deark_module_info *mi)
{
	mi->id = "wri";
	mi->desc = "Microsoft Write";
	mi->run_fn = de_run_wri;
	mi->identify_fn = de_identify_wri;
}
