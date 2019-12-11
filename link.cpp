/* c++17 */

#include <algorithm>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

/* old version of stdlib have this stuff in utility */
#if __has_include(<charconv>)
#define HAVE_CHARCONV
#include <charconv>
#endif

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <err.h>
#include <sysexits.h>
#include <unistd.h>

#include <afp/finder_info.h>


#include "mapped_file.h"

#include "omf.h"
#include "rel.h"

void save_omf(const std::string &path, std::vector<omf::segment> &segments, bool compress, bool expressload);
int set_file_type(const std::string &path, uint16_t file_type, uint32_t aux_type, std::error_code &ec);
void set_file_type(const std::string &path, uint16_t file_type, uint32_t aux_type);

/* since span isn't standard yet */
typedef std::basic_string_view<uint8_t> byte_view;


struct symbol {
	std::string name;
	std::string file;
	uint32_t value = 0;
	unsigned id = 0;
	unsigned count = 0;

	bool absolute = false;
	bool defined = false;
};


std::unordered_map<std::string, unsigned> symbol_map;
std::vector<symbol> symbol_table;

struct pending_reloc : public omf::reloc {
	unsigned id = 0;
};

std::vector<pending_reloc> relocations;


std::vector<omf::segment> segments;

/* nb - pointer may be invalidated by next call */
symbol *find_symbol(const std::string &name) {
	
	auto iter = symbol_map.find(name);
	if (iter != symbol_map.end()) return &symbol_table[iter->second];

	unsigned id = symbol_table.size();
	symbol_map.emplace(name, id);

	auto &rv = symbol_table.emplace_back();
	rv.name = name;
	rv.id = id;
	return &rv;
}



struct cookie {
	std::string file;
	std::vector<unsigned> remap;
	//std::vector<std::pair<unsigned, unsigned>> zero;

	uint32_t begin = 0;
	uint32_t end = 0;
};

void process_labels(byte_view &data, cookie &cookie) {

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


void process_reloc(byte_view &data, cookie &cookie) {

	auto &seg = segments.back();

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
			relocations.emplace_back(r);
		} else {
			omf::reloc r;
			r.size = size;
			r.offset = offset;
			r.value = value + cookie.begin;
			r.shift = shift;

			seg.relocs.emplace_back(r);
		}
		/* clear out the inline relocation data */
		for(unsigned i = 0; i < size; ++i) {
			seg.data[offset + i] = 0;
		}
		//cookie.zero.emplace_back(std::make_pair(offset, size));
	}
}

/*
void add_libraries() {
	auto iter = libs.begin();
	auto end = libs.end();

	for(;;) {



	}
}
*/


void process_unit(const std::string &path) {

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

	omf::segment &seg = segments.back();

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
}


void resolve(void) {

	/* this needs to be updated if supporting multiple segments */
	auto &seg = segments.back();

	for (auto &r : relocations) {
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

		r.value += e.value;
		seg.relocs.emplace_back(r);
	}
	relocations.clear();

	/* sort them */
	std::sort(seg.relocs.begin(), seg.relocs.end(), [](const omf::reloc &a, const omf::reloc &b){
		return a.offset < b.offset;
	});
}

static void print_symbols2(void) {

	for (const auto &e : symbol_table) {
		char q = ' ';
		if (!e.count) q = '?';
		if (!e.defined) q = '!';
		fprintf(stdout, "%c %-20s=$%06x\n", q, e.name.c_str(), e.value);
	}	
}

void print_symbols(void) { 

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



void usage(int ex) {

	fputs("merlin-link [-o outfile] infile...\n", stderr);
	exit(ex);
}

/* older std libraries lack charconv and std::from_chars */
bool parse_number(const char *begin, const char *end, uint32_t &value, int base = 10) {

#if defined(HAVE_CHARCONV)
	auto r =  std::from_chars(begin, end, value, base);
	if (r.ec != std::errc() || r.ptr != end) return false;
#else
	auto xerrno = errno;
	errno = 0;
	char *ptr = nullptr;
	value = std::strtoul(begin, &ptr, base);
	std::swap(errno, xerrno);
	if (xerrno || ptr != end) {
		return false;
	}
#endif

	return true;
}

static void add_define(std::string str) {
	/* -D key[=value]
 		value = 0x, $, % or base 10 */

	uint32_t value = 0;

	auto ix = str.find('=');
	if (ix == 0) usage(EX_USAGE);
	if (ix == str.npos) {
		value = 1;
	} else {

		int base = 10;
		auto pos = ++ix;

		char c = str[pos]; /* returns 0 if == size */

		switch(c) {
			case '%':
				base = 2; ++pos; break;
			case '$':
				base = 16; ++pos; break;
			case '0':
				c = str[pos+1];
				if (c == 'x' || c == 'X') {
					base = 16; pos += 2;					
				}
				break;
		}
		if (!parse_number(str.data() + pos, str.data() + str.length(), value, base))
			usage(EX_USAGE);

		str.resize(ix-1);
	}


	symbol *e = find_symbol(str);
	if (e->defined && e->absolute && e->value == value) return;

	if (e->defined) {
		warnx("%s previously defined", str.c_str());
		return;
	}

	e->defined = true;
	e->absolute = true;
	e->file = "-D";
	e->value = value;
}


int main(int argc, char **argv) {

	int c;
	std::string gs_out = "gs.out";
	bool express = true;
	bool compress = true;


	while ((c = getopt(argc, argv, "o:D:XC")) != -1) {
		switch(c) {
			case 'o':
				gs_out = optarg;
				break;
			case 'X': express = false; break;
			case 'C': compress = false; break;
			case 'D': add_define(optarg); break;
			case ':':
			case '?':
			default:
				usage(EX_USAGE);
				break;
		}
	}

	argv += optind;
	argc -= optind;

	if (!argc) usage(EX_USAGE);

	segments.emplace_back();
	for (int i = 0; i < argc; ++i) {
		char *path = argv[i];
		try {
			process_unit(path);
		} catch (std::exception &ex) {
			errx(EX_DATAERR, "%s: %s", path, ex.what());
		}
	}

	resolve();
	print_symbols();

	try {
		save_omf(gs_out, segments, compress, express);
		set_file_type(gs_out, 0xb3, 0x0000);
		exit(0);
	} catch (std::exception &ex) {
		errx(EX_OSERR, "%s: %s", gs_out.c_str(), ex.what());
	}
}
