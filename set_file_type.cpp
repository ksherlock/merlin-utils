#include <string>
#include <stdint.h>
#include <system_error>
#include <afp/finder_info.h>


int set_file_type(const std::string &path, uint16_t file_type, uint32_t aux_type, std::error_code &ec) {

	afp::finder_info fi;

	if (!fi.open(path, afp::finder_info::read_write, ec))
		return -1;

	fi.set_prodos_file_type(file_type, aux_type);
	if (!fi.write(ec))
		return -1;

	return 0;
}

void set_file_type(const std::string &path, uint16_t file_type, uint32_t aux_type) {
	std::error_code ec;
	if (set_file_type(path, file_type, aux_type, ec) < 0)
		throw std::system_error(ec);
}
