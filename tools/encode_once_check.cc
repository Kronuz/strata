// encode_once_check TODAY.sx ENCODE_ONCE.sx: compares two segments of the same documents,
// today's flush and the encode-once flush (encode_once_check.hh), and prints their sizes.
#include "encode_once_check.hh"

#include <fstream>
#include <iostream>
#include <sstream>

static std::string slurp(const char* path) {
	std::ifstream f(path, std::ios::binary);
	std::ostringstream s;
	s << f.rdbuf();
	return s.str();
}

int main(int argc, char** argv) {
	if (argc != 3) {
		std::cerr << "usage: encode_once_check TODAY.sx ENCODE_ONCE.sx\n";
		return 2;
	}
	const std::string a = slurp(argv[1]), b = slurp(argv[2]);
	try {
		(void)encode_once_check::compare(a, b, true);
	} catch (const std::exception& e) {
		std::cerr << "MISMATCH: " << e.what() << "\n";
		return 1;
	}
	std::printf("OK: every list equal (today's decoded by the reader too); other sections byte-identical\n");
	return 0;
}
