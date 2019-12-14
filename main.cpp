
#include <string_view>
#include <string>
#include <utility>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>

/* old version of stdlib have this stuff in utility */
#if __has_include(<charconv>)
#define HAVE_CHARCONV
#include <charconv>
#endif

#include <err.h>
#include <sysexits.h>
#include <unistd.h>

#include "link.h"

static void usage(int ex) {

	fputs(
		"merlin-link [options] infile...\n"
		"\noptions:\n"
		"-C              inhibit SUPER compression\n"
		"-D symbol=value define symbol\n"
		"-X              inhibit expressload segment\n"
		"-o outfile      specify output file (default gs.out)\n"
		"-v              be verbose\n"
		"\n",
		stderr);

	exit(ex);
}

/* older std libraries lack charconv and std::from_chars */
static bool parse_number(const char *begin, const char *end, uint32_t &value, int base = 10) {

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


	define(str, value, LBL_D);
}

/* .ends_with() is c++20 */
static bool is_S(std::string_view sv) {
	size_t s = sv.size();
	// && is a sequence point.
	return s >= 2 && std::toupper(sv[--s]) == 'S' && sv[--s] == '.';
}


bool verbose = false;
std::string save_file = "gs.out";
bool express = true;
bool compress = true;

int main(int argc, char **argv) {

	int c;
	bool script = false;

	while ((c = getopt(argc, argv, "o:D:XCSv")) != -1) {
		switch(c) {
			case 'o':
				save_file = optarg;
				break;
			case 'X': express = false; break;
			case 'C': compress = false; break;
			case 'D': add_define(optarg); break;
			case 'v': verbose = true; break;
			case 'S': script = true; break;
			case ':':
			case '?':
			default:
				usage(EX_USAGE);
				break;
		}
	}

	argv += optind;
	argc -= optind;

	if (!script && !argc) usage(EX_USAGE);
	if (script && argc > 1) usage(EX_USAGE);
	if (argc == 1 && is_S(*argv)) script = true;

	if (script) process_script(argc ? *argv : nullptr);
	else process_files(argc, argv);

	exit(0);
}