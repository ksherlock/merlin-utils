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
	unsigned segment = 0;
	unsigned count = 0;

	bool absolute = false;
	bool defined = false;
};

/*

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

	EXT		n	n	y << imports from linker to command

*/
enum {
	LBL_EQU = (1 << 0),
	LBL_GEQ = (1 << 0) | (1 << 1) | (1 << 2),
	LBL_KBD = (1 << 0) | (1 << 1) | (1 << 2),
	LBL_D   = (1 << 0) | (1 << 1) | (1 << 2),
	LBL_EQ  = (1 << 2),
	LBL_POS = (1 << 1),
	LBL_LEN = (1 << 1),

	LBL_EXT = (1 << 2)
};

void process_script(const char *argv);
void process_files(int argc, char **argv);


symbol *find_symbol(const std::string &name, bool insert = true);

void define(std::string name, uint32_t value, int type);


#endif