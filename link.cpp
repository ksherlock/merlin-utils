/* c++17 */

#include <algorithm>
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
};


namespace {


	std::unordered_map<std::string, unsigned> symbol_map;
	std::vector<symbol> symbol_table;

	std::vector<omf::segment> segments;
	std::vector<std::vector<pending_reloc>> relocations;


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
	unsigned kind = 0x0000;
	unsigned sav = 0;
	bool end = false;

	size_t pos_offset = 0;
	size_t len_offset = 0;

	/* do/els/fin stuff */
	uint32_t active_bits = 1;
	bool active = true;

	std::unordered_map<std::string, uint32_t> local_symbol_table; 

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



static void process_labels(byte_view &data, cookie &cookie) {

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
				e->segment = segments.size();
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
			assert((flag  & ~(0x0f|0x10|0x20|0x80)) == 0);

			// offset already adjusted by start so below comparisons are wrong.
			switch(flag & (0x80 + 0x20)) {
				case 0:
					size = 1;
					assert(offset + 0 < cookie.end);
					break;
				case 0x20:
					size = 3;
					assert(offset + 2 < cookie.end);
					break;
				case 0x80:
					size = 2;
					assert(offset + 1 < cookie.end);
					break;
				default: /* bad size */
					errx(1, "%s: Unsupported flag: %02x\n", cookie.file.c_str(), flag);
					break;
			}
			external = flag & 0x10;

			switch(size) {
				case 3: value |= seg.data[offset+2] << 16;
				case 2: value |= seg.data[offset+1] << 8;
				case 1: value |= seg.data[offset+0];
			}


			if (size > 1) value -= 0x8000;

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
		/* clear out the inline relocation data */
		for (unsigned i = 0; i < size; ++i) {
			seg.data[offset + i] = 0;
		}
		//cookie.zero.emplace_back(std::make_pair(offset, size));
	}
}


static void process_unit(const std::string &path) {

	cookie cookie;
	/* skip over relocs, do symbols first */


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

	// LEN support
	len_offset = offset;
}


static void resolve(void) {

	for (unsigned seg_num = 0; seg_num < segments.size(); ++seg_num) {

		auto &seg = segments[seg_num];
		auto &pending = relocations[seg_num];


		for (auto &r : pending) {
			assert(r.id < symbol_map.size());
			const auto &e = symbol_table[r.id];

			/* if this is an absolute value, do the math */
			if (!e.defined) {
				warnx("%s is not defined", e.name.c_str());
				continue;
			}

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

			if (e.segment == seg_num) {
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
	}
}

static void print_symbols2(void) {

	for (const auto &e : symbol_table) {
		char q = ' ';
		if (!e.count) q = '?';
		if (!e.defined) q = '!';
		fprintf(stdout, "%c %-20s=$%06x\n", q, e.name.c_str(), e.value);
	}	
}

static void print_symbols(void) { 

	if (symbol_table.empty()) return;

	/* alpha */
	fputs("\nSymbol table, alphabetical order:\n", stdout);
	std::sort(symbol_table.begin(), symbol_table.end(),
		[](const symbol &a, const symbol &b){
			return a.name < b.name; 
		});

	print_symbols2();

	fputs("\nSymbol table, numerical order:\n", stdout);

	/* numeric */
	std::sort(symbol_table.begin(), symbol_table.end(),
		[](const symbol &a, const symbol &b){
			return a.value < b.value; 
		});

	print_symbols2();

}




void finish(void) {

	resolve();
	print_symbols();

	try {
		save_omf(save_file, segments, compress, express);
		set_file_type(save_file, ftype, atype);
	} catch (std::exception &ex) {
		errx(EX_OSERR, "%s: %s", save_file.c_str(), ex.what());
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
			return true;
		default:
			return false;
	}
}

static uint32_t eval(operand_t op) {
	if (std::holds_alternative<uint32_t>(op)) return std::get<uint32_t>(op);
	std::string &name = std::get<std::string>(op);
	auto iter = local_symbol_table.find(name);
	if (iter == local_symbol_table.end()) throw std::runtime_error("Bad symbol");
	return iter->second;
}


void evaluate(label_t label, opcode_t opcode, operand_t operand) {

	// todo - should move operand parsing to here.

	switch(opcode) {
		case OP_DO:
			if (active_bits & 0x80000000) throw std::runtime_error("too much do do");
			active_bits <<= 1;
			active_bits |= eval(operand) ? 1 : 0;
			active = (active_bits & (active_bits + 1)) == 0;
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
			if (!end && lkv == 2) {
				/* finish up */
				segments.pop_back();
				relocations.pop_back();
				if (!segments.empty())
					finish();
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


		case OP_TYP:
			// todo - should evaluate with file type dictionary.
			ftype = eval(operand);
			break;
		case OP_ADR:
			atype = eval(operand);
			break;

		case OP_KND:
			kind = eval(operand);
			break;

		case OP_LKV: {
			/* specify linker version */
			/* 0 = binary, 1 = Linker.GS, 2 = Linker.XL, 3 = convert to OMF object file */

			uint32_t value = eval(operand);
			switch (value) {
				case 0: throw std::runtime_error("binary linker not supported");
				case 3: throw std::runtime_error("object file linker not supported");
				case 1:
				case 2:
					lkv = value;
					break;
				default:
					throw std::runtime_error("bad linker version");
			}
			break;
		}

		case OP_VER: {
			/* OMF version, 1 or 2 */
			uint32_t value = eval(operand);

			if (value != 2)
				throw std::runtime_error("bad OMF version");
			ver = value;
			break;
		}

		case OP_LNK:
			if (end) throw std::runtime_error("link after end");
			//link_unit(std::get<std::string>(operand));
			break;

		case OP_SAV:
			if (end) throw std::runtime_error("save after end");
			if (save_file.empty()) save_file = std::get<std::string>(operand);

			/* if linker version 1, save to disk */
			/* if linker version 2, finish the current segment */
			if (lkv == 1) {
				auto &seg = segments.back();
				seg.segname = std::get<std::string>(operand);
				seg.kind = kind;
				finish();
				end = true;
			}
			if (lkv == 2) {
				auto &seg = segments.back();
				seg.segname = std::get<std::string>(operand);
				seg.kind = kind;

				/* add a new segment */
				segments.emplace_back();
				relocations.emplace_back();
				pos_offset = 0; // POS support
			}
			++sav;
			break;

		case OP_KBD: {
			std::string prompt;
			char buffer[256];

			if (!isatty(STDIN_FILENO)) return;

			/* todo if already defined (via -D) don't prompt */
			if (local_symbol_table.find(label) != local_symbol_table.end())
				return;

			if (std::holds_alternative<std::string>(operand)) 
				prompt = std::get<std::string>(operand);

			if (prompt.empty()) prompt = "Give value for " + label;
			prompt += ": ";
			fputs(prompt.c_str(), stdout);
			fflush(stdout);

			char *cp = fgets(buffer, sizeof(buffer), stdin);

			if (!cp) return;

			operand_t number_operand(const char *YYCURSOR, bool required);
			operand_t op = number_operand(cp, true);

			define(label, eval(op), LBL_KBD);
			break;
		}

		case OP_POS: {
			// POS label << sets label = current segment offset
			// POS  << resets pos byte counter.

			if (std::holds_alternative<std::monostate>(operand)) {
				pos_offset = segments.back().data.size();
			} else {
				std::string label = std::get<std::string>(operand);
				uint32_t value = segments.back().data.size() - pos_offset;

				define(label, value, LBL_POS);
			}
			break;
		}
		case OP_LEN: {
			// LEN label
			// sets label = length of most recent file linked
			std::string label = std::get<std::string>(operand);
			uint32_t value = len_offset;
			define(label, value, LBL_LEN);
			break;
		}

		case OP_EQ:
			define(label, eval(operand), LBL_EQ);
			break;
		case OP_EQU:
			define(label, eval(operand), LBL_EQU);
			break;
		case OP_GEQ:
			define(label, eval(operand), LBL_GEQ);
			break;

		case OP_FAS:
			/* fast linker, only 1 file allowed */
		case OP_OVR:
		case OP_PUT:
		case OP_IF:
			break;

		case OP_ASM:
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


	segments.emplace_back();
	relocations.emplace_back();

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

	segments.emplace_back();
	relocations.emplace_back();

	for (int i = 0; i < argc; ++i) {
		char *path = argv[i];
		try {
			process_unit(path);
		} catch (std::exception &ex) {
			errx(EX_DATAERR, "%s: %s", path, ex.what());
		}
	}
}

