#ifndef script_h
#define script_h

#include <cstdint>
#include <string>

typedef std::string label_t;

enum opcode_t {
	
	OP_NONE = 0,
	#define x(op) OP_##op,
	#include "ops.h"
	#undef x
	OP_EQ
};

enum {
	OVR_NONE = 1,
	OVR_ALL = -1,
	OVR_OFF = 0
};

enum {
	OP_OPTIONAL = 0,
	OP_REQUIRED = 1,
	OP_INSENSITIVE = 2
};

#endif
