/* c++17 */

#include <algorithm>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <err.h>
#include <sysexits.h>
#include <unistd.h>

#include <afp/finder_info.h>


#include "mapped_file.h"

#include "omf.h"
#include "rel.h"
#include "link.h"
#include "script.h"

void save_omf(const std::string &path, std::vector<omf::segment> &segments, bool compress, bool expressload);
void save_bin(const std::string &path, omf::segment &segment, uint32_t org);

int set_file_type(const std::string &path, uint16_t file_type, uint32_t aux_type, std::error_code &ec);
void set_file_type(const std::string &path, uint16_t file_type, uint32_t aux_type);

/* since span isn't standard yet */
typedef std::basic_string_view<uint8_t> byte_view;



struct pending_reloc : public omf::reloc {
	unsigned id = 0;
};



struct cookie {
	std::string file;
	std::vector<unsigned> remap;

	uint32_t begin = 0;
	uint32_t end = 0;

	unsigned ds_fill = 0;
};


namespace {


	std::unordered_map<std::string, unsigned> symbol_map;
	std::vector<symbol> symbol_table;

	std::vector<omf::segment> segments;
	std::vector<std::vector<pending_reloc>> relocations;

	std::unordered_map<std::string, uint32_t> file_types = {

		{ "NON", 0x00 },
		{ "BAD", 0x01 },
		{ "BIN", 0x06 },
		{ "TXT", 0x04 },
		{ "DIR", 0x0f },
		{ "ADB", 0x19 },
		{ "AWP", 0x1a },
		{ "ASP", 0x1b },
		{ "GSB", 0xab },
		{ "TDF", 0xac },
		{ "BDF", 0xad },
		{ "SRC", 0xb0 },
		{ "OBJ", 0xb1 },
		{ "LIB", 0xb2 },
		{ "S16", 0xb3 },
		{ "RTL", 0xb4 },
		{ "EXE", 0xb5 },
		{ "PIF", 0xb6 },
		{ "TIF", 0xb7 },
		{ "NDA", 0xb8 },
		{ "CDA", 0xb9 },
		{ "TOL", 0xba },
		{ "DRV", 0xbb },
		{ "DOC", 0xbf },
		{ "PNT", 0xc0 },
		{ "PIC", 0xc1 },
		{ "FON", 0xcb },
		{ "PAS", 0xef },
		{ "CMD", 0xf0 },
		{ "LNK", 0xf8 },
		{ "BAS", 0xfc },
		{ "VAR", 0xfd },
		{ "REL", 0xfe },
		{ "SYS", 0xff },

	};

}


/*
 Variable types:

	linker symbol table includes =, EQU, GEQ, and KBD


	GEQ - global absolute label, in effect for all subsequent asms.
	inhibits KBD, otherwise causes duplicate symbol errors during assembly.

	KBD - same as GEQ

	EQU - same as GEQ BUT symbol is discarded after ASM (ie, only in effect for 1 assembly)

	=  - internal to link script (DO, etc). not passed to assembler. not passed to linker.


	POS - current offset
	LEN - length of last linked file

a = assembler
l = linker
c = command file

			a	l	c
	EQU		y	n	n
	=		n	n	y
	GEQ		y	y	y
	KBD		y	y	y
	POS		n	y	n
	LEN		n	y	n

seems like it might be nice for POS and LEN to be available in the command file, eg

	POS xxx
	DO xxx>4096
	ERR too big
	ELS
	DS 4096-xxx
	FIN

 */


namespace {
	/* script related */

	unsigned lkv = 1;
	unsigned ver = 2;
	unsigned ftype = 0xb3;
	unsigned atype = 0x0000;
//	unsigned kind = 0x0000;
	unsigned org = 0x0000;

	unsigned sav = 0;
	unsigned lnk = 0;
	bool end = false;
	bool fas = false;
	int ovr = OVR_OFF;

	size_t pos_var = 0;
	size_t len_var = 0;

	/* do/els/fin stuff.  32 do levels supported. */
	uint32_t active_bits = 1;
	bool active = true;

	std::unordered_map<std::string, uint32_t> local_symbol_table; 

	std::string loadname;
}


/* nb - pointer may be invalidated by next call */
symbol *find_symbol(const std::string &name, bool insert) {
	
	auto iter = symbol_map.find(name);
	if (iter != symbol_map.end()) return &symbol_table[iter->second];
	if (!insert) return nullptr;

	unsigned id = symbol_table.size();
	symbol_map.emplace(name, id);

	auto &rv = symbol_table.emplace_back();
	rv.name = name;
	rv.id = id;
	return &rv;
}

void define(std::string name, uint32_t value, int type) {

	bool warn = false;
	if (type & 4) {
		/* command script */
		auto iter = local_symbol_table.find(name);
		if (iter == local_symbol_table.end()) {
			local_symbol_table.emplace(std::make_pair(name, value));
		} else if (iter->second != value) {
			warn = true;
		}
	}
	if (type & 2) {
		/* linker */
		auto e = find_symbol(name, true);
		if (e->defined) {
			if (!e->absolute || e->value != value) {
				warn = true;
			}
		} else {
			e->absolute = true;
			e->defined = true;
			e->file = "-D";
			e->value = value;
		}
	}
	if (warn) warnx("duplicate symbol %s", name.c_str());

}

void new_segment(bool reset = false) {

	if (reset) {
		segments.clear();
		relocations.clear();
		save_file.clear();
	}

	segments.emplace_back();
	relocations.emplace_back();

	segments.back().segnum = segments.size();
	segments.back().kind = 4096; /* no special memory */
	len_var = 0;
	pos_var = 0;
}


static void process_labels(byte_view &data, cookie &cookie) {

	unsigned segnum = segments.back().segnum;
	for(;;) {
		assert(data.size());
		unsigned flag = data[0];
		if (flag == 0x00) return;

		unsigned length = flag & 0x1f;
		assert(length != 0);
		assert(data.size() >= length + 4);

		std::string name(data.data() + 1, data.data() + 1 + length);
		data.remove_prefix(1 + length);
		uint32_t value = data[0] | (data[1] << 8) | (data[2] << 16);
		data.remove_prefix(3);

		symbol *e = find_symbol(name);
		switch (flag & ~0x1f) {
			case SYMBOL_EXTERNAL:
				/* map the unit symbol # to a global symbol # */
				if (!(value & 0x8000)) e->exd = true;

				value &= 0x7fff;
				if (cookie.remap.size() < value + 1)
					cookie.remap.resize(value + 1);
				cookie.remap[value] = e->id;
				break;


			case SYMBOL_ENTRY+SYMBOL_ABSOLUTE:
				if (e->defined && e->absolute && e->value == value)
					break; /* allow redef */

			case SYMBOL_ENTRY:
				if (e->defined) {
					warnx("%s previously defined (%s)", e->name.c_str(), e->file.c_str());
					break;
				}
				e->defined = true;
				e->file = cookie.file;
				e->segment = segnum;
				if (flag & SYMBOL_ABSOLUTE) {
					e->absolute = true;
					e->value = value;
				} else {
					e->absolute = false;
					e->value = value - 0x8000 + cookie.begin;
				}
				break;
			default:
				errx(1, "%s: Unsupported flag: %02x\n", cookie.file.c_str(), flag);
				break;
		}
	}
}


static void process_reloc(byte_view &data, cookie &cookie) {

	auto &seg = segments.back();
	auto &pending = relocations.back();

	for(;;) {
		assert(data.size());
		unsigned flag = data[0];
		if (flag == 0x00) return;

		assert(data.size() >= 4);

		uint32_t offset = data[1] | (data[2] << 8);
		unsigned x = data[3];
		data.remove_prefix(4);

		offset += cookie.begin;
		bool external = false;
		bool ddb = false;

		unsigned shift = 0;
		uint32_t value = 0;
		unsigned size = 0;

		if (flag == 0xff) {
			/* shift */
			assert(data.size() >= 4);
			unsigned flag = data[0];
			value = data[1] | (data[2] << 8) | (data[3] << 16);
			value -= 0x8000;
			external = flag & 0x04;
			switch(flag & ~0x04) {
				case 0xd0:
					shift = -16;
					size = 1;
					break;
				case 0xd1:
					shift = -8;
					size = 2;
					break;
				case 0xd3:
					shift = -8;
					size = 1;
					break;
				default: /* bad */
					errx(1, "%s: Unsupported flag: %02x\n", cookie.file.c_str(), flag);
					break;
			}
			data.remove_prefix(4);
		} else {

			// offset already adjusted by start so below comparisons are wrong.
			switch(flag & 0xf0) {
				case 0x00:
				case 0x10:
					size = 1;
					break;
				case 0x20:
				case 0x30:
					size = 3;
					break;
				case 0x40:
					size = 1;
					shift = -8;
					break;
				case 0x80:
				case 0x90:
					size = 2;
					break;
				case 0xa0:
				case 0xb0:
					/* ddb */
					size = 2;
					ddb = true;
					break;
				case 0xc0:
					/* $cf - ds fill */
					if (!cookie.ds_fill) cookie.ds_fill = x | 0x0100;
					return;
				default: /* bad size */
					errx(1, "%s: Unsupported flag: %02x\n", cookie.file.c_str(), flag);
					break;
			}
			external = flag & 0x10;

			assert(offset + size  <= cookie.end);


			switch(size) {
				case 3: value |= seg.data[offset+2] << 16;
				case 2: value |= seg.data[offset+1] << 8;
				case 1: value |= seg.data[offset+0];
			}

			if (ddb) value = ((value >> 8) | (value << 8)) & 0xffff;

			if (flag & 0x40) {
				/* value is already shifted, so need to adjust back */
				value <<= 8;
				value += x; /* low-byte of address */
				value -= 0x8000;
				assert(!external);
			}
			if (size > 1) value -= 0x8000;

		}

		/* clear out the inline relocation data */
		for (unsigned i = 0; i < size; ++i) {
			seg.data[offset + i] = 0;
		}

		if (ddb) {
			/*
			 * ddb - data is stored inline in big-endian format.
			 * generate 1-byte, -8 shift for offset+0
			 * generate 1-byte, 0 shift for offset+1
			 */


			if (external) {
				pending_reloc r;
				assert(x < cookie.remap.size());
				r.id = cookie.remap[x];
				r.size = 1;
				r.offset = offset;
				r.value = value;
				r.shift = -8;

				symbol_table[r.id].count += 1;
				pending.emplace_back(r);

				pending.emplace_back(r);
				r.offset++;
				r.shift = 0;
				pending.emplace_back(r);

			} else {

				omf::reloc r;
				r.size = 1;
				r.offset = offset;
				r.value = value + cookie.begin;
				r.shift = -8;

				seg.relocs.emplace_back(r);

				r.offset++;
				r.shift = 0;
				seg.relocs.emplace_back(r);
			}
			return;
		}

		/* external resolutions are deferred for later */
		if (external) {
			/* x = local symbol # */
			pending_reloc r;
			assert(x < cookie.remap.size());
			r.id = cookie.remap[x];
			r.size = size;
			r.offset = offset;
			r.value = value;
			r.shift = shift;

			symbol_table[r.id].count += 1;
			pending.emplace_back(r);
		} else {
			omf::reloc r;
			r.size = size;
			r.offset = offset;
			r.value = value + cookie.begin;
			r.shift = shift;

			seg.relocs.emplace_back(r);
		}

		//cookie.zero.emplace_back(std::make_pair(offset, size));
	}
}


static void process_unit(const std::string &path) {

	cookie cookie;
	/* skip over relocs, do symbols first */

	if (verbose) printf("Linking %s\n", path.c_str());

	std::error_code ec;
	mapped_file mf(path, mapped_file::readonly, ec);
	if (ec) {
		errx(1, "Unable to open %s: %s", path.c_str(), ec.message().c_str());
	}


	afp::finder_info fi;

	fi.read(path, ec);

	if (ec) {
		errx(1, "Error reading filetype %s: %s", path.c_str(), ec.message().c_str());
	}

	if (fi.prodos_file_type() != 0xf8) {
		errx(1, "Wrong file type: %s", path.c_str());
	}

	uint32_t offset = fi.prodos_aux_type();
	if (offset+2 > mf.size()) {
		errx(1, "Invalid aux type %s", path.c_str());
	}

	auto &seg = segments.back();

	cookie.begin = seg.data.size();
	cookie.end = cookie.begin + offset;
	cookie.file = path;

	seg.data.insert(seg.data.end(), mf.data(), mf.data() + offset);
	byte_view data(mf.data() + offset, mf.size() - offset);


	byte_view rr = data;
	/* skip over the relocation records so we can process the labels first. */
	/* this is so external references can use the global symbol id */
	assert(data.size() >= 2);
	for(;;) {
		if (data[0] == 0) break;
		assert(data.size() >= 6);
		data.remove_prefix(4);
	}
	data.remove_prefix(1);
	process_labels(data, cookie);
	assert(data.size() == 1);

	/* now relocations */
	process_reloc(rr, cookie);

	if (cookie.ds_fill) {
		/* per empirical merlin testing,
			LEN/POS opcodes not affected by DS \ fills.
		 */
		unsigned sz = (256ul - seg.data.size()) & 0xff;
		if (sz) {
			seg.data.insert(seg.data.end(), sz, cookie.ds_fill & 0xff);
		}
	}

	// LEN support
	len_var = offset;
	pos_var += offset;
}


static void import(const std::string &path, const std::string &name) {

	std::error_code ec;
	mapped_file mf(path, mapped_file::readonly, ec);
	if (ec) {
		errx(1, "Unable to open %s: %s", path.c_str(), ec.message().c_str());
	}

	auto &seg = segments.back();

	// check for duplicate label.
	auto e = find_symbol(name);
	if (e->defined) {
		warnx("Duplicate symbol %s", name.c_str());
		return;
	}

	e->file = path;
	e->defined = true;
	e->value = seg.data.size();
	e->segment = segments.back().segnum;

	seg.data.insert(seg.data.end(), mf.data(), mf.data() + mf.size());

	// LEN support
	len_var = mf.size();
	pos_var += mf.size();
}

static void resolve(bool allow_unresolved = false) {

	for (unsigned ix = 0; ix < segments.size(); ++ix) {

		auto &seg = segments[ix];
		auto &pending = relocations[ix];

		std::vector<pending_reloc> unresolved;

		if ((seg.kind & 0x0001) == 0x0001 && seg.data.size() > 65535) {
			throw std::runtime_error("code exceeds bank");
		}

		for (auto &r : pending) {
			assert(r.id < symbol_map.size());
			const auto &e = symbol_table[r.id];

			if (!e.defined) {
				if (allow_unresolved) {
					unresolved.emplace_back(std::move(r));
				} else {
					warnx("%s is not defined", e.name.c_str());
				}
				continue;
			}

			/* if this is an absolute value, do the math */
			if (e.absolute) {
				uint32_t value = e.value + r.value;
				/* shift is a uint8_t so negating doesn't work right */
				value >>= -(int8_t)r.shift;

				unsigned offset = r.offset;
				unsigned size = r.size;
				while (size--) {
					seg.data[offset++] = value & 0xff;
					value >>= 8;
				}
				continue;
			}

			if (e.segment == seg.segnum) {
				r.value += e.value;
				seg.relocs.emplace_back(r);
				continue;
			}

			omf::interseg inter;
			inter.size = r.size;
			inter.shift = r.shift;
			inter.offset = r.offset;
			inter.segment = e.segment;
			inter.segment_offset = r.value + e.value;

			seg.intersegs.emplace_back(inter);
		}
		pending.clear();

		/* sort them */
		std::sort(seg.relocs.begin(), seg.relocs.end(), [](const auto &a, const auto &b){
			return a.offset < b.offset;
		});

		std::sort(seg.intersegs.begin(), seg.intersegs.end(), [](const auto &a, const auto &b){
			return a.offset < b.offset;
		});

		std::sort(unresolved.begin(), unresolved.end(), [](const auto &a, const auto &b){
			return a.offset < b.offset;
		});
		pending = std::move(unresolved);
	}
}


static void print_symbols2(const std::vector<size_t> &ix) {

	size_t len = 8;
	for (const auto &e : symbol_table) {
		len = std::max(len, e.name.size());
	}

	for (auto i : ix) {
		const auto &e = symbol_table[i];
		char q = ' ';
		if (!e.count) q = '?';
		if (!e.defined) q = '!';
		uint32_t value = e.value;
		if (!e.absolute) value += (e.segment << 16);
		fprintf(stdout, "%c %-*s=$%06x\n", q, (int)len, e.name.c_str(), value);
	}	
}

static void print_symbols(void) { 

	if (symbol_table.empty()) return;

	std::vector<size_t> ix(symbol_table.size());
	std::iota(ix.begin(), ix.end(), 0);

	/* alpha */
	fputs("\nSymbol table, alphabetical order:\n", stdout);


	std::sort(ix.begin(), ix.end(), [&](const size_t a, const size_t b){
		const symbol &aa = symbol_table[a];
		const symbol &bb = symbol_table[b];

		return aa.name < bb.name;
	});

#if 0
	std::sort(symbol_table.begin(), symbol_table.end(),
		[](const symbol &a, const symbol &b){
			return a.name < b.name; 
		});
#endif
	print_symbols2(ix);

	std::iota(ix.begin(), ix.end(), 0);

	fputs("\nSymbol table, numerical order:\n", stdout);

	/* numeric, factoring in segment #, absolute first */

	std::sort(ix.begin(), ix.end(), [&](const size_t a, const size_t b){
		const symbol &aa = symbol_table[a];
		const symbol &bb = symbol_table[b];

			/* absolute have a segment # of 0 so will sort first */
			auto aaa = std::make_pair(aa.segment, aa.value);
			auto bbb = std::make_pair(bb.segment, bb.value);

			return aaa < bbb;

	});

#if 0
	std::sort(symbol_table.begin(), symbol_table.end(),
		[](const symbol &a, const symbol &b){
			/* absolute have a segment # of 0 so will sort first */
			auto aa = std::make_pair(a.segment, a.value);
			auto bb = std::make_pair(b.segment, b.value);

			return aa < bb;
		});
#endif
	print_symbols2(ix);
	fputs("\n", stdout);
}


static void check_exd(void) {

	for (const auto &e : symbol_table) {

		if (!e.exd) continue;
		if (!e.defined) continue;
		if (e.absolute && e.value < 0x0100) continue;
		if (!e.absolute && lkv == 0 && (e.value + org) < 0x0100) continue;

		warnx("%s defined as direct page", e.name.c_str());
	}
}


void finish(void) {

	resolve();

	std::string path = save_file;

	if (path.empty()) path = "omf.out";

	if (verbose) printf("Saving %s\n", path.c_str());
	try {
		if (lkv == 0)
			save_bin(path, segments.back(), org);
		else
			save_omf(path, segments, compress, express);

		set_file_type(path, ftype, atype);
	} catch (std::exception &ex) {
		errx(EX_OSERR, "%s: %s", path.c_str(), ex.what());
	}

	check_exd();

	segments.clear();
	relocations.clear();
}

namespace {

	void push(std::vector<uint8_t> &v, uint8_t x) {
		v.push_back(x);
	}

	void push(std::vector<uint8_t> &v, uint16_t x) {
		v.push_back(x & 0xff);
		x >>= 8;
		v.push_back(x & 0xff);
	}

	void push(std::vector<uint8_t> &v, uint32_t x) {
		v.push_back(x & 0xff);
		x >>= 8;
		v.push_back(x & 0xff);
		x >>= 8;
		v.push_back(x & 0xff);
		x >>= 8;
		v.push_back(x & 0xff);
	}

	void push(std::vector<uint8_t> &v, const std::string &s) {
		uint8_t count = std::min((int)s.size(), 255);
		push(v, count);
		v.insert(v.end(), s.begin(), s.begin() + count);
	}

	void push(std::vector<uint8_t> &v, const std::string &s, size_t count) {
		std::string tmp(s, 0, count);
		tmp.resize(count, ' ');
		v.insert(v.end(), tmp.begin(), tmp.end());
	}

}

static void add_expr(std::vector<uint8_t> &buffer, const omf::reloc &r, int ix) {

	push(buffer, omf::opcode::EXPR);
	push(buffer, static_cast<uint8_t>(r.size));

	if (ix >= 0) {
		/* external */
		push(buffer, static_cast<uint8_t>(0x83)); /* label reference */
		push(buffer, symbol_table[ix].name);

		if (r.value) {

			push(buffer, static_cast<uint8_t>(0x81)); /* abs */
			push(buffer, static_cast<uint32_t>(r.value));
			push(buffer, static_cast<uint8_t>(0x01)); /* + */
		}
	} else {
		push(buffer, static_cast<uint8_t>(0x87)); /* rel */
		push(buffer, static_cast<uint32_t>(r.value));
	}

	if (r.shift){
		push(buffer, static_cast<uint8_t>(0x81)); /* abs */
		push(buffer, static_cast<uint32_t>(static_cast<int8_t>(r.shift)));
		push(buffer, static_cast<uint8_t>(0x07)); /* << */
	}

	push(buffer, static_cast<uint8_t>(0)); /* end of expr */

}

/* REL to OMF object file */
/* relocations and labels need to be placed inline */
void finish3(void) {

	resolve(true); /* allow unresolved references */

	std::vector< std::pair<uint32_t, std::string> > globals;

	auto &seg = segments.back();
	auto &unresolved = relocations.back();
	auto &resolved = seg.relocs;
	auto &data = seg.data;

	std::vector<uint8_t> buffer;
	/* 1. generate GEQU for all global equates */
	for (const auto &sym : symbol_table) {
		if (sym.defined) {
			if (sym.absolute) {

				push(buffer, omf::opcode::GEQU);
				push(buffer, sym.name);
				push(buffer, static_cast<uint16_t>(0x00)); /* length attr */
				push(buffer, static_cast<uint8_t>('G')); /* type attr */
				push(buffer, static_cast<uint32_t>(sym.value));
			} else {
				globals.emplace_back(sym.value, sym.name);
			}
		}
	}
	std::sort(globals.begin(), globals.end());



	auto iter1 = globals.begin();
	auto iter2 = unresolved.begin();
	auto iter3 = resolved.begin();

	std::vector<unsigned> breaks;
	for (const auto &x : globals) {
		breaks.push_back(x.first);
	}
	for (const auto &x : resolved) {
		breaks.push_back(x.offset);
	}
	for (const auto &x : unresolved) {
		breaks.push_back(x.offset);
	}
	/* sort in reverse order */
	std::sort(breaks.begin(), breaks.end(), std::greater<unsigned>());
	breaks.erase(std::unique(breaks.begin(), breaks.end()), breaks.end());


	unsigned pc = 0;
	unsigned offset = 0;
	for(;;) {
		unsigned next = data.size();

		while (!breaks.empty() && breaks.back() < offset) breaks.pop_back();

		if (!breaks.empty()) {
			next = std::min(next, breaks.back());
			breaks.pop_back();
		}

		if (next < offset)
			throw std::runtime_error("relocation offset error");

		unsigned size = next - offset;
		if (size) {
			if (size <= 0xdf)
				push(buffer, static_cast<uint8_t>(size));
			else {
				push(buffer, omf::opcode::LCONST);
				push(buffer, static_cast<uint32_t>(size));
			}
			while (offset < next) buffer.push_back(data[offset++]);
			pc += size;
		}


		/* global expr global expr */
		for(;;) {
			bool delta = false;
			while (iter1 != globals.end() && iter1->first == offset) {
				/* add global record */
				push(buffer, omf::opcode::GLOBAL);
				push(buffer, iter1->second); /* name */
				push(buffer, static_cast<uint16_t>(0x00)); /* length attr */
				push(buffer, static_cast<uint8_t>('N')); /* type attr */
				push(buffer, static_cast<uint8_t>(0x00)); /* public */
				++iter1;
			}

			if (iter2 != unresolved.end() && iter2->offset == offset) {
				const auto &r = *iter2;
				add_expr(buffer, r, r.id);
				offset += r.size;
				pc += r.size;
				delta = true;
				++iter2;
			}
			if (iter3 != resolved.end() && iter3->offset == offset) {
				const auto &r = *iter3;
				add_expr(buffer, r, -1);
				offset += r.size;
				pc += r.size;
				delta = true;
				++iter3;
			}
			if (!delta) break;
		}
		if (offset >= data.size()) break;
	}

	push(buffer, omf::opcode::END);
	seg.data = std::move(buffer);

	if (iter1 != globals.end())
		throw std::runtime_error("label offset error");
	if (iter2 != unresolved.end())
		throw std::runtime_error("relocation offset error");
	if (iter3 != resolved.end())
		throw std::runtime_error("relocation offset error");

	void save_object(const std::string &path, omf::segment &s, uint32_t length);


	std::string path = save_file;
	if (path.empty()) path = "omf.out";
	if (verbose) printf("Saving %s\n", path.c_str());

	try {
		save_object(path, seg, pc);
		set_file_type(path, 0xb1, 0x0000);
	} catch (std::exception &ex) {
		errx(EX_OSERR, "%s: %s", path.c_str(), ex.what());
	}

	print_symbols();
	segments.clear();
	relocations.clear();
}

void lib(const std::string &path) {

	/* for all unresolved symbols, link path/symbol ( no .L extension) */

	std::string p = path;
	if (!p.empty() && p.back() != '/') p.push_back('/');
	auto size = p.size();

	/* symbol table might reallocate so can't use for( : ) loop */
	/* any new dependencies will be appended at the end and processed */
	for (size_t i = 0; i < symbol_table.size(); ++i) {

		auto &e = symbol_table[i];

		if (e.absolute || e.defined) continue;

		p.append(e.name);

		/* check the file type... */
		std::error_code ec;
		afp::finder_info fi;

		fi.read(path, ec);

		if (ec || fi.prodos_file_type() != 0xf8) continue;
		process_unit(p);
		p.resize(size);
		// assume e is invalid at this point.
	}
}

static bool op_needs_label(opcode_t op) {
	switch (op) {
		case OP_KBD:
		case OP_EQ:
		case OP_EQU:
		case OP_GEQ:
			return true;
		default:
			return false;
	}
}

static bool op_after_end(opcode_t op) {
	switch(op) {
		case OP_END:
		case OP_CMD:
		case OP_PFX:
		case OP_DAT:
		case OP_RES:
		case OP_RID:
		case OP_RTY:
		case OP_RAT:
		case OP_FIL:
			return true;
		default:
			return false;
	}
}


extern uint32_t number_operand(const char *cursor, int flags = OP_REQUIRED);
extern uint32_t number_operand(const char *cursor, const std::unordered_map<std::string, uint32_t> &, int flags = OP_REQUIRED);
extern int ovr_operand(const char *cursor);
extern std::string label_operand(const char *cursor, int flags = OP_REQUIRED);
extern std::string string_operand(const char *cursor, int flags = OP_REQUIRED);
extern std::string path_operand(const char *cursor, int flags = OP_REQUIRED);

extern void no_operand(const char *cursor);

static std::string basename(const std::string &str) {

	auto ix = str.find_last_of("/:");
	if (ix == str.npos) return str;
	return str.substr(0, ix);
}

/* fixup GS/OS strings. */
static void fix_path(std::string &s) {
	for (char &c : s)
		if (c == ':') c = '/';
}

/*
 SEG name -> undocumented? command to set the OMF segment name (linker 3 only)

 */
void evaluate(label_t label, opcode_t opcode, const char *cursor) {

	// todo - should move operand parsing to here.

	switch(opcode) {
		case OP_DO:
			if (active_bits & 0x80000000) throw std::runtime_error("too much do do");
			active_bits <<= 1;
			if (active) {
				uint32_t value = number_operand(cursor, local_symbol_table);
				active_bits |= value ? 1 : 0;
				active = (active_bits & (active_bits + 1)) == 0;
			}
			return;
			break;

		case OP_ELS:
			if (active_bits < 2)
				throw std::runtime_error("els without do");

			active_bits ^= 0x01;
			active = (active_bits & (active_bits + 1)) == 0;
			return;
			break;

		case OP_FIN:
			active_bits >>= 1;
			if (!active_bits) {
				active = 1;
				throw std::runtime_error("fin without do");
			}
			active = (active_bits & (active_bits + 1)) == 0;
			return;
			break;
		default:
			break;
	}
	if (!active) return;

	if (label.empty() && op_needs_label(opcode))
			throw std::runtime_error("Bad label");

	if (end && !op_after_end(opcode)) return;

	switch(opcode) {

		case OP_END:
			if (lkv == 2) {
				/* finish up */
				segments.pop_back();
				relocations.pop_back();
				if (!segments.empty())
					finish();
				// reset.  could have another link afterwards.
				new_segment(true);
			}
			end = true;
			break;

		case OP_DAT: {
			/* 29-DEC-88   4:18:37 PM */
			time_t t = time(nullptr);
			struct tm *tm = localtime(&t);
			char buffer[32];

			strftime(buffer, sizeof(buffer), "%d-%b-%y  %l:%M:%S %p", tm);
			for (char &c : buffer) c = std::toupper(c);

			fprintf(stdout, "%s\n", buffer);
			break;
		}

		case OP_PFX: {

			std::string path = path_operand(cursor);
			fix_path(path);

			int ok = chdir(path.c_str());
			if (ok < 0)
				warn("PFX %s", path.c_str());
			break;
		}


		case OP_TYP:
			ftype = number_operand(cursor, file_types, OP_REQUIRED | OP_INSENSITIVE);
			break;
		case OP_ADR:
			atype = number_operand(cursor, local_symbol_table);
			break;

		case OP_ORG:
			org = number_operand(cursor, local_symbol_table);
			atype = org;
			break;

		case OP_KND: {
			uint32_t kind = number_operand(cursor, local_symbol_table);
			if (!segments.empty())
				segments.back().kind = kind;
			break;
		}
		case OP_ALI: {
			uint32_t align = number_operand(cursor, local_symbol_table);
			// must be power of 2 or 0
			if (align & (align-1))
				throw std::runtime_error("Bad alignment");

			segments.back().alignment = align;
			break;
		}

		case OP_DS: {
			// todo - how is this handled in binary linker?
			uint32_t ds = number_operand(cursor, local_symbol_table);
			segments.back().reserved_space = ds;
			break;
		}


		case OP_LKV: {
			/* specify linker version */
			/* 0 = binary, 1 = Linker.GS, 2 = Linker.XL, 3 = convert to OMF object file */

			uint32_t value = number_operand(cursor, local_symbol_table);
			switch (value) {
				case 0:
				case 1:
				case 2:
				case 3:
					lkv = value;
					break;
				default:
					throw std::runtime_error("bad linker version");
			}
			break;
		}

		case OP_VER: {
			/* OMF version, 1 or 2 */
			uint32_t value = number_operand(cursor, local_symbol_table);

			if (value != 2)
				throw std::runtime_error("bad OMF version");
			ver = value;
			break;
		}

		case OP_LNK: {
			if (end) throw std::runtime_error("link after end");

			std::string path = path_operand(cursor);
			process_unit(path);
			++lnk;
			break;
		}

		case OP_IMP: {

			/* qasm addition. import binary file. entry name is filename w/ . converted to _ */
			std::string path = path_operand(cursor);
			std::string name = basename(path);
			for (char &c : name) {
				c = std::isalnum(c) ? std::toupper(c) : '_';
			}
			import(path, name);
			++lnk;
			break;
		}

		case OP_SAV: {
			if (end) throw std::runtime_error("save after end");

			std::string path = path_operand(cursor);
			std::string base = basename(path);
			auto &seg = segments.back();

			/* use 1st SAV as the path */
			if (save_file.empty()) save_file = path;
			if (loadname.empty()) loadname = base;

			/*
				lkv 0 = binary linker
				lkv 1 = 1 segment GS linker
				lkv 2 = multi-segment GS linker
				lkv 3 = convert REL to OMF object file
			 */

			if (lkv == 1 || lkv == 2 || lkv == 3) {
				/* merlin link uses a 10-char fixed label */
				//base.resize(10, ' ');
				seg.segname = base;
				seg.loadname = loadname;
				// seg.kind = kind;
			}

			switch (lkv) {
				case 0:
				case 1:
					finish();
					new_segment(true);
					break;
				case 2:
					if (verbose) printf("Segment %d: %s\n", seg.segnum, base.c_str());
					/* add a new segment */
					new_segment();
					break;
				case 3:
					finish3();
					new_segment(true);
					break;			
			}

			++sav;
			break;
		}

		case OP_ENT:
			print_symbols();
			break;

		case OP_KBD: {
			char buffer[256];

			if (!isatty(STDIN_FILENO)) return;

			/* todo if already defined (via -D) don't prompt */
			if (local_symbol_table.find(label) != local_symbol_table.end())
				return;

			std::string prompt = string_operand(cursor, OP_OPTIONAL);

			if (prompt.empty()) prompt = "Give value for " + label;
			prompt += ": ";
			fputs(prompt.c_str(), stdout);
			fflush(stdout);

			char *cp = fgets(buffer, sizeof(buffer), stdin);

			if (!cp) throw std::runtime_error("Bad input");

			uint32_t value = number_operand(cp, local_symbol_table, true);

			define(label, value, LBL_KBD);
			break;
		}

		case OP_POS: {
			// POS label << sets label = current segment offset
			// POS  << resets pos byte counter.

			std::string label = label_operand(cursor, OP_OPTIONAL);
			if (label.empty()) {
				pos_var = 0;
			} else {
				define(label, pos_var, LBL_POS);
			}
			break;
		}
		case OP_LEN: {
			// LEN label
			// sets label = length of most recent file linked

			std::string label = label_operand(cursor);
			uint32_t value = len_var;
			define(label, value, LBL_LEN);
			break;
		}

		case OP_EQ:
			define(label, number_operand(cursor, local_symbol_table), LBL_EQ);
			break;
		case OP_EQU:
			define(label, number_operand(cursor, local_symbol_table), LBL_EQU);
			break;
		case OP_GEQ:
			define(label, number_operand(cursor, local_symbol_table), LBL_GEQ);
			break;

		case OP_EXT: {
			/* no label is a no-op. */
			if (label.empty()) break;

			/* otherwise, it imports an absolute label into the local symbol table */
			auto e = find_symbol(label, false);
			if (!e || !e->absolute) throw std::runtime_error("Bad address");
			define(label, e->value, LBL_EXT);

			break;
		}

		case OP_SEG: {
			/* OMF object file linker - set the object file seg name */
			std::string name = label_operand(cursor);
			break;
		}

		case OP_FAS:
			/* fast linker, only 1 file allowed */
			fas = true;
			break;

		case OP_OVR:
			ovr = ovr_operand(cursor);
			break;

		case OP_PUT: {
			std::string path = path_operand(cursor);
			break;
		}
		case OP_IF: {
			std::string path = path_operand(cursor);
			break;
		}

		case OP_ASM: {
			std::string path = path_operand(cursor);
			break;			
		}
		default:
			throw std::runtime_error("opcode not yet supported");		
	}

}

void process_script(const char *path) {

	extern void parse_line(const char *);

	FILE *fp = nullptr;

	if (!path || !strcmp(path, "-")) fp = stdin;
	else {
		fp = fopen(path, "r");
		if (!fp) {
			err(1, "Unable to open %s", path);
		}
	}


	new_segment();

	int no = 1;
	int errors = 0;
	char *line = NULL;
	size_t cap = 0;
	for(;; ++no) {

		ssize_t len = getline(&line, &cap, fp);
		if (len == 0) break;
		if (len < 0) break;

		/* strip trailing ws */
		while (len && isspace(line[len-1])) --len;
		line[len] = 0;
		if (len == 0) continue; 

		try {
			parse_line(line);
		} catch (std::exception &ex) {
			if (!active) continue;

			fprintf(stderr, "%s in line: %d\n", ex.what(), no);
			fprintf(stderr, "%s\n", line);
			if (++errors >= 10) {
				fputs("Too many errors, aborting\n", stderr);
				break;
			}
		}
	}
	if (fp != stdin)
		fclose(fp);
	free(line);
	exit(errors ? EX_DATAERR : 0);
}




void process_files(int argc, char **argv) {

	new_segment();

	for (int i = 0; i < argc; ++i) {
		char *path = argv[i];
		try {
			process_unit(path);
		} catch (std::exception &ex) {
			errx(EX_DATAERR, "%s: %s", path, ex.what());
		}
	}
	finish();
	if (verbose) print_symbols();
	exit(0);
}

