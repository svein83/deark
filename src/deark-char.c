// This file is part of Deark, by Jason Summers.
// This software is in the public domain. See the file COPYING for details.

// deark-char.c
//
// Functions related to character graphics.

#include "deark-config.h"
#include "deark-private.h"

struct charextractx {
	de_byte used_blink;
	de_byte used_fgcol[16];
	de_byte used_bgcol[16];
};

static void do_prescan_screen(deark *c, struct de_char_context *charctx,
	struct charextractx *ectx, struct de_char_screen *screen)
{
	const struct de_char_cell *cell;
	int i, j;
	de_byte cell_fgcol_actual;

	for(j=0; j<screen->height; j++) {
		for(i=0; i<screen->width; i++) {
			if(!screen->cell_rows || !screen->cell_rows[j]) continue;
			cell = &screen->cell_rows[j][i];
			if(!cell) continue;

			cell_fgcol_actual = cell->fgcol;
			if(cell->bold) cell_fgcol_actual |= 0x08;

			if(cell->fgcol<16) ectx->used_fgcol[(unsigned int)cell_fgcol_actual] = 1;
			if(cell->bgcol<16) ectx->used_bgcol[(unsigned int)cell->bgcol] = 1;
			if(cell->blink) ectx->used_blink = 1;
		}
	}
}

static void do_output_screen(deark *c, struct de_char_context *charctx,
	struct charextractx *ectx, struct de_char_screen *screen, dbuf *ofile)
{
	const struct de_char_cell *cell;
	int i, j;
	de_int32 n;
	int span_count = 0;
	int need_newline = 0;

	de_byte active_fgcol = 0;
	de_byte active_bgcol = 0;
	de_byte active_blink = 0;
	de_byte cell_fgcol_actual;

	dbuf_fputs(ofile, "<table style=\"margin-left:auto;margin-right:auto\"><tr>\n<td>");
	dbuf_fputs(ofile, "<pre>");
	for(j=0; j<screen->height; j++) {
		for(i=0; i<screen->width; i++) {
			if(!screen->cell_rows || !screen->cell_rows[j]) continue;
			cell = &screen->cell_rows[j][i];
			if(!cell) continue;

			cell_fgcol_actual = cell->fgcol;
			if(cell->bold) cell_fgcol_actual |= 0x08;

			if(span_count==0 || cell_fgcol_actual!=active_fgcol || cell->bgcol!=active_bgcol ||
				cell->blink!=active_blink)
			{
				while(span_count>0) {
					dbuf_fprintf(ofile, "</span>");
					span_count--;
				}

				if(need_newline) {
					dbuf_fputs(ofile, "\n");
					need_newline = 0;
				}

				dbuf_fputs(ofile, "<span class=\"");

				// Classes for foreground and background colors
				dbuf_fprintf(ofile, "f%c", de_get_hexchar(cell_fgcol_actual));
				dbuf_fprintf(ofile, " b%c", de_get_hexchar(cell->bgcol));

				// Other attributes
				if(cell->blink) dbuf_fputs(ofile, " blink");

				dbuf_fputs(ofile, "\">");

				span_count++;
				active_fgcol = cell_fgcol_actual;
				active_bgcol = cell->bgcol;
				active_blink = cell->blink;
			}

			n = cell->codepoint;
			if(n==0x00) n=0x20;
			if(n<0x20) n='?';

			if(need_newline) {
				dbuf_fputs(ofile, "\n");
				need_newline = 0;
			}

			de_write_codepoint_to_html(c, ofile, n);
		}

		// Defer emitting a newline, so that we have more control over where
		// to put it. We prefer to put it after "</span>".
		need_newline = 1;
	}

	while(span_count>0) {
		dbuf_fprintf(ofile, "</span>");
		span_count--;
	}

	dbuf_fputs(ofile, "</pre>");
	dbuf_fputs(ofile, "</td>\n</tr></table>\n");
}

static void output_css_color_block(deark *c, dbuf *ofile, de_uint32 *pal,
	const char *selectorprefix, const char *prop, const de_byte *used_flags)
{
	char tmpbuf[16];
	int i;

	for(i=0; i<16; i++) {
		if(!used_flags[i]) continue;
		de_color_to_css(pal[i], tmpbuf, sizeof(tmpbuf));
		dbuf_fprintf(ofile, " %s%c { %s: %s }\n", selectorprefix, de_get_hexchar(i),
			prop, tmpbuf);
	}
}

static void do_output_header(deark *c, struct de_char_context *charctx,
	struct charextractx *ectx, dbuf *ofile)
{
	if(c->write_bom && !c->ascii_html) dbuf_write_uchar_as_utf8(ofile, 0xfeff);
	dbuf_fputs(ofile, "<!DOCTYPE html>\n");
	dbuf_fputs(ofile, "<html>\n");
	dbuf_fputs(ofile, "<head>\n");
	if(!c->ascii_html) dbuf_fputs(ofile, "<meta charset=\"UTF-8\">\n");
	dbuf_fputs(ofile, "<title></title>\n");

	dbuf_fputs(ofile, "<style type=\"text/css\">\n");

	dbuf_fputs(ofile, " body { background-image: url(\"data:image/png;base64,"
		"iVBORw0KGgoAAAANSUhEUgAAABAAAAAQAQMAAAAlPW0iAAAABlBMVEUgICAoKCidji3LAAAAMUlE"
		"QVQI12NgaGBgPMDA/ICB/QMD/w8G+T8M9v8Y6v8z/P8PIoFsoAhQHCgLVMN4AACOoBFvDLHV4QAA"
		"AABJRU5ErkJggg==\") }\n");

	output_css_color_block(c, ofile, charctx->pal, ".f", "color", &ectx->used_fgcol[0]);
	output_css_color_block(c, ofile, charctx->pal, ".b", "background-color", &ectx->used_bgcol[0]);

	if(ectx->used_blink) {
		dbuf_fputs(ofile, " .blink {\n"
			"  animation: blink 1s steps(1) infinite;\n"
			"  -webkit-animation: blink 1s steps(1) infinite }\n"
			" @keyframes blink { 50% { color: transparent } }\n"
			" @-webkit-keyframes blink { 50% { color: transparent } }\n");
	}
	dbuf_fputs(ofile, "</style>\n");

	dbuf_fputs(ofile, "</head>\n");
	dbuf_fputs(ofile, "<body>\n");
}

static void do_output_footer(deark *c, struct de_char_context *charctx,
	struct charextractx *ectx, dbuf *ofile)
{
	dbuf_fputs(ofile, "</body>\n</html>\n");
}

void de_char_output_to_file(deark *c, struct de_char_context *charctx)
{
	dbuf *ofile = NULL;
	de_int64 i;
	struct charextractx *ectx = NULL;

	ectx = de_malloc(c, sizeof(struct charextractx));

	for(i=0; i<charctx->nscreens; i++) {
		do_prescan_screen(c, charctx, ectx, charctx->screens[i]);
	}

	ofile = dbuf_create_output_file(c, "html", NULL);

	do_output_header(c, charctx, ectx, ofile);
	for(i=0; i<charctx->nscreens; i++) {
		do_output_screen(c, charctx, ectx, charctx->screens[i], ofile);
	}
	do_output_footer(c, charctx, ectx, ofile);

	dbuf_close(ofile);
	de_free(c, ectx);
}
