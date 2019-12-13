#ifndef link_h
#define link_h

#include <string>
#include <cstdint>

extern bool verbose;
extern bool compress;
extern bool express;
extern std::string save_file;


struct symbol {
	std::string name;
	std::string file;
	uint32_t value = 0;
	unsigned id = 0;
	unsigned count = 0;

	bool absolute = false;
	bool defined = false;
};


void process_script(const char *argv);
void process_files(int argc, char **argv);


symbol *find_symbol(const std::string &name, bool insert = true);

#endif