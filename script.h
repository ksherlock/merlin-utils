#ifndef script_h
#define script_h

#include <cstdint>
#include <string>
#include <variant>

typedef std::string label_t;
typedef std::variant<std::monostate, uint32_t, std::string> operand_t;

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

#endif
