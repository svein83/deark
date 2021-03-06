// This file is part of Deark.
// Copyright (C) 2018 Jason Summers
// See the file COPYING for terms of use.

// AppleDouble, etc.

#include <deark-config.h>
#include <deark-private.h>
DE_DECLARE_MODULE(de_module_applesingle);
DE_DECLARE_MODULE(de_module_appledouble);

typedef struct localctx_struct {
	u32 version;
	struct de_timestamp modtime;
	de_ucstring *real_name;
} lctx;

struct entry_id_struct;

struct entry_struct {
	unsigned int idx;
	unsigned int id;
	i64 offset;
	i64 length;
	const struct entry_id_struct *eid;
};

typedef void (*handler_fn_type)(deark *c, lctx *d, struct entry_struct *e);

struct entry_id_struct {
	unsigned int id;
	const char *name;
	handler_fn_type hfn;
};

// len = the total number of bytes available
static void read_pascal_string(deark *c, lctx *d, de_ucstring *s, i64 pos, i64 len)
{
	i64 slen;

	if(len<1) goto done;
	slen = (i64)de_getbyte(pos);
	if(slen<1 || slen > (len-1)) goto done;
	dbuf_read_to_ucstring(c->infile, pos+1, slen, s, 0, DE_ENCODING_MACROMAN);
done:
	;
}

static void handler_string(deark *c, lctx *d, struct entry_struct *e)
{
	de_ucstring *s = NULL;

	s = ucstring_create(c);
	read_pascal_string(c, d, s, e->offset, e->length);
	de_dbg(c, "%s: \"%s\"", e->eid->name, ucstring_getpsz_d(s));

	if(e->id==3 && !d->real_name && s->len>0) { // id 3 = real name
		d->real_name = ucstring_clone(s);
	}

	ucstring_destroy(s);
}

static void do_one_date(deark *c, lctx *d, i64 pos, const char *name,
	int is_modtime)
{
	i64 dt;
	char timestamp_buf[64];

	dt = de_geti32be(pos);
	if(dt == -0x80000000LL) {
		de_strlcpy(timestamp_buf, "unknown", sizeof(timestamp_buf));
	}
	else {
		struct de_timestamp ts;
		// Epoch is Jan 1, 2001. There are 30 years, with 7 leap days, between
		// that and the Unix time epoch.
		de_unix_time_to_timestamp(dt + ((365*30 + 7)*86400), &ts, 0x1);
		if(is_modtime) {
			d->modtime = ts; // struct copy
		}
		de_timestamp_to_string(&ts, timestamp_buf, sizeof(timestamp_buf), 0);
	}
	de_dbg(c, "%s: %"I64_FMT" (%s)", name, dt, timestamp_buf);
}

static void handler_dates(deark *c, lctx *d, struct entry_struct *e)
{
	if(e->length<16) return;
	do_one_date(c, d, e->offset, "creation date", 0);
	do_one_date(c, d, e->offset+4, "mod date", 1);
	do_one_date(c, d, e->offset+8, "backup date", 0);
	do_one_date(c, d, e->offset+12, "access date", 0);
}

static void handler_data(deark *c, lctx *d, struct entry_struct *e)
{
	de_finfo *fi = NULL;

	fi = de_finfo_create(c);
	if(d->modtime.is_valid) {
		fi->mod_time = d->modtime; // struct copy
	}

	if(d->real_name) {
		de_finfo_set_name_from_ucstring(c, fi, d->real_name, 0);
		fi->original_filename_flag = 1;
	}
	else {
		de_finfo_set_name_from_sz(c, fi, "data", 0, DE_ENCODING_LATIN1);
	}

	dbuf_create_file_from_slice(c->infile, e->offset, e->length,
		NULL, fi, 0x0);

	de_finfo_destroy(c, fi);
}

static void handler_rsrc(deark *c, lctx *d, struct entry_struct *e)
{
	de_finfo *fi = NULL;
	de_ucstring *fname = NULL;

	if(e->length<1) goto done;

	fi = de_finfo_create(c);
	if(d->modtime.is_valid) {
		fi->mod_time = d->modtime; // struct copy
	}

	if(d->real_name) {
		fname = ucstring_clone(d->real_name);
		ucstring_append_sz(fname, ".rsrc", DE_ENCODING_LATIN1);
		de_finfo_set_name_from_ucstring(c, fi, fname, 0);
	}
	else {
		de_finfo_set_name_from_sz(c, fi, "rsrc", 0, DE_ENCODING_LATIN1);
	}

	dbuf_create_file_from_slice(c->infile, e->offset, e->length,
		NULL, fi, 0x0);

done:
	de_finfo_destroy(c, fi);
	ucstring_destroy(fname);
}

static const struct entry_id_struct entry_id_arr[] = {
	{1, "data fork", handler_data},
	{2, "resource fork", handler_rsrc},
	{3, "real name", handler_string},
	{4, "comment", handler_string},
	{5, "b/w icon", NULL},
	{6, "color icon", NULL},
	{8, "file dates", handler_dates},
	{9, "Finder info", NULL},
	{10, "Macintosh file info", NULL},
	{11, "ProDOS file info", NULL},
	{12, "MS-DOS file info", NULL},
	{13, "short name", NULL},
	{14, "AFP file info", NULL},
	{15, "directory ID", NULL}
};

static const struct entry_id_struct *find_entry_id_info(unsigned int id)
{
	size_t k;

	for(k=0; k<DE_ITEMS_IN_ARRAY(entry_id_arr); k++) {
		if(entry_id_arr[k].id==id) return &entry_id_arr[k];
	}
	return NULL;
}

static void do_sd_entry(deark *c, lctx *d, unsigned int idx, i64 pos1)
{
	struct entry_struct e;
	const struct entry_id_struct *eid;
	i64 pos = pos1;

	de_zeromem(&e, sizeof(struct entry_struct));
	e.idx = idx;
	e.id = (unsigned int)de_getu32be_p(&pos);
	eid =  find_entry_id_info(e.id);
	de_dbg(c, "id: %u (%s)", e.id, eid?eid->name:"?");
	e.offset = de_getu32be_p(&pos);
	de_dbg(c, "offset: %"I64_FMT, e.offset);
	e.length = de_getu32be_p(&pos);
	de_dbg(c, "length: %"I64_FMT, e.length);

	if(e.offset > c->infile->len) goto done;
	if(e.offset+e.length > c->infile->len) {
		de_warn(c, "Entry %u goes beyond end of file. Reducing size from %"I64_FMT
			" to %"I64_FMT".", e.idx, e.length, c->infile->len-e.offset);
		e.length = c->infile->len - e.offset;
	}

	if(eid && eid->hfn) {
		e.eid = eid;
		eid->hfn(c, d, &e);
	}

done:
	;
}

static void de_run_sd_internal(deark *c, lctx *d)
{
	i64 pos = 0;
	i64 nentries;
	i64 k;
	int pass;
	i64 entry_descriptors_pos;
	int *entry_pass = NULL;

	pos += 4; // signature
	d->version = (u32)de_getu32be_p(&pos);
	de_dbg(c, "version: 0x%08x", (unsigned int)d->version);
	pos += 16; // filler

	nentries = de_getu16be_p(&pos);
	de_dbg(c, "number of entries: %d", (int)nentries);

	entry_descriptors_pos = pos;

	entry_pass = de_mallocarray(c, nentries, sizeof(int));
	for(k=0; k<nentries; k++) {
		unsigned int e_id;
		// Make sure we read the metadata before we extract the files.
		e_id = (unsigned int)de_getu32be(entry_descriptors_pos+12*k);
		if(e_id==1 || e_id==2) entry_pass[k] = 2;
		else entry_pass[k] = 1;
	}

	for(pass=1; pass<=2; pass++) {
		for(k=0; k<nentries; k++) {
			if(entry_pass[k]==pass) {
				if(pos+12>c->infile->len) break;
				de_dbg(c, "entry[%u]", (unsigned int)k);
				de_dbg_indent(c, 1);
				do_sd_entry(c, d, (unsigned int)k, entry_descriptors_pos+12*k);
				de_dbg_indent(c, -1);
			}
		}
	}

	de_free(c, entry_pass);
	de_free(c, d->real_name);
}

static void de_run_appledouble(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	de_run_sd_internal(c, d);
	de_free(c, d);
}

static int de_identify_appledouble(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x00\x05\x16\x07", 4))
		return 100;
	return 0;
}

void de_module_appledouble(deark *c, struct deark_module_info *mi)
{
	mi->id = "appledouble";
	mi->desc = "AppleDouble Header file";
	mi->run_fn = de_run_appledouble;
	mi->identify_fn = de_identify_appledouble;
}

static void de_run_applesingle(deark *c, de_module_params *mparams)
{
	lctx *d = NULL;

	d = de_malloc(c, sizeof(lctx));
	de_run_sd_internal(c, d);
	de_free(c, d);
}

static int de_identify_applesingle(deark *c)
{
	if(!dbuf_memcmp(c->infile, 0, "\x00\x05\x16\x00", 4))
		return 100;
	return 0;
}

void de_module_applesingle(deark *c, struct deark_module_info *mi)
{
	mi->id = "applesingle";
	mi->desc = "AppleSingle";
	mi->run_fn = de_run_applesingle;
	mi->identify_fn = de_identify_applesingle;
}
