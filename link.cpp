/* c++17 */

#include <vector>
#include <unordered_map>
#include <string>

#include "omf.h"

void save_omf(const std::string &path, std::vector<omf::segment> &segments, bool compress, bool expressload);


struct symbol {
	std::string name;
	std::string file;
	uint32_t value = 0;
	unsigned id = 0;
	unsigned count = 0;

	bool absolute = false;
	bool defined = false;
};

struct pending_reloc : public omf::reloc {
	unsigned id = 0;
};

std::unordered_map<std::string, unsigned> symbol_map;
std::vector<symbol> symbol_table;

reference *find_symbol(const std::string &name) {
	
	auto iter = symbol_map.find(name);
	if (iter != symbol_map.end()) return &symbol_table[*iter - 1];

	unsigned id = symbol_table.size() + 1;
	symbol_map.emplace(name, id);

	auto &rv = symbol_table.emplace_back();
	rv.name = name;
	rv.id = id;
	return *rv;
}



uint32_t start = 0; /* starting point of current unit */
std::vector<unsigned> remap;


process_labels(std::span &data) {

	for(;;) {
		assert(data.size())
		unsigned flag = data[0];
		if (flag == 0x00) return;

		unsigned length = flag & 0x1f;
		assert(length != 0);
		assert(data.size() >= length + 4);

		std::string name(data.data() + 1, data.data() + 1 + length);
		data.remove_prefix(1 + length);
		uint32_t value = data[0] | (data[1] << 8) | (data[2] << 16);
		data.remove_prefix(3);

		reference *e = find_symbol(name);
		switch (flag & ~0x1f) {
			case SYMBOL_EXTERNAL:
				/* map the unit symbol # to a global symbol # */
				value &= 0x7fff;
				if (remap.size() < value + 1)
					remap.resize(value + 1);
				remap[value] = e->id;
				break;


			case SYMBOL_ENTRY+SYMBOL_ABSOLUTE:
				if (e->defined && e->absolute && e->value == value)
					break; /* allow redef */

			case SYMBOL_ENTRY:
				if (e->defined) {
					warnx("%s previously defined (%s)", e->name, e->file);
					break;
				}
				e->defined = true;
				e->file = file;
				if (flag & SYMBOL_ABSOLUTE) {
					e->absolute = true;
					e->value = value;
				} else {
					e->absolute = false;
					e->value = value - 0x8000 + start;
				}
				break;
			default:
		}
	}
}


process_reloc(std::span &data) {

	for(;;) {
		assert(data.size());
		unsigned flag = data[0];
		if (flag == 0x00) return;

		assert(data.size() >= 4);

		uint32_t offset = data[1] | (data[2] << 8);
		unsigned x = data[3];
		data.remove_prefix(4);

		offset += start;
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
					shift = 16;
					size = 1;
					break;
				case 0xd1:
					shift = 8;
					size = 2;
					break;
				case 0xd3:
					shift = 8;
					size = 1;
					break;
				default: /* bad */
			}
		} else {
			assert(flag  & ~(0x0f|0x10|0x20|0x80) == 0);

			unsigned size = 0;
			switch(flag & (0x80 + 0x20)) {
				case 0: size = 1;
				case 0x20: size = 3;
				case 0x80: size = 2;
				default: /* bad size */
			}
			external = flag & 0x10;

			switch(size) {
				case 3: value |= seg_data[offset+2] << 16;
				case 2: value |= seg_data[offset+1] << 8;
				case 1: value |= seg_data[offset+0];
			}


			if (size > 1) value -= 0x8000;
			value += start;

		}

		/* external resolutions are deferred for later */

		if (external) {
			/* x = local symbol # */
			reloc r;
			assert(x < remap.size());
			r.id = remap[x]; /* label reference is 0-based */
			r.size = size;
			r.offset = offset;
			r.value = value;
			r.shift = shift;

			relocations.emplace_back(r);
		} else {
			uint32_t value = 0;
			omf::reloc r;
			r.size = size;
			r.offset = start;
			r.value = value;
			r.shift = shift;

			seg.relocs.emplace_back(r);
		}

	}
}

void add_libraries() {
	auto iter = libs.begin();
	auto end = libs.end();

	for(;;) {



	}
}



process_unit(span data) {

	/* skip over relocs, do symbols first */
	remap.clear();

	span rr = data;
	for(;;) {
		if (data[0] == 0) break;
		data.remove_prefix(4);
	}
	data.remove_prefix(1);
	process_labels(data);
	assert(data.length() == 1);

	/* now relocations */
	process_reloc(rr);
}


finalize(void) {

	for (auto &r in relocations) {
		assert(r.id <= symbol_map.length());
		const auto &label = symbol_map[rr.id];

		r.value = label.value;
		seg.relocs.emplace_back(r);
	}
	relocations.clear();
}

void print_symbols(void) {

	if (symbol_table.empty()) return;

	/* alpha */
	std::sort(symbol_table.begin(), symbol_table.end(),
		[](const symbol &a, const symbol &b){
			return a.name < b.name; 
		});

	for (const auto &lab : symbol_table) {
		fprintf(stdout, "%-20s: $%06x\n", lab.name.c_str(), lab.value);
	}
	fputs("\n", stdout);
	/* numeric */
	std::sort(symbol_table.begin(), symbol_table.end(),
		[](const symbol &a, const symbol &b){
			return a.value < b.value; 
		});

	for (const auto &lab : symbol_table) {
		fprintf(stdout, "%-20s: $%06x\n", lab.name.c_str(), lab.value);
	}
}