#include "index.h"

int main(int argc, char **argv) {
	if (argc == 1)
		atire_index("-?");
	return atire_index(argv[1]);
}
