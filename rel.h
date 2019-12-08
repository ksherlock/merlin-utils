#ifndef REL_H
#define REL_H

enum {
	SYMBOL_ABSOLUTE = 0x20,
	SYMBOL_ENTRY = 0x40,
	SYMBOL_EXTERNAL = 0x80,
};

enum {
	FLAG_EXTERNAL = 0x10,
	FLAG_3_BYTE = 0x20,
	FLAG_2_BYTE = 0x80,
	FLAG_SHIFT = 0xff,
};

enum {
	SHIFT_16_1 = 0xd0,
	SHIFT_8_1 = 0xd3,
	SHIFT_8_2 = 0xd1,

	SHIFT_EXTERNAL = 0x04,
};

#endif
