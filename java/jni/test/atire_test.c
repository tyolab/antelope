/*
 * atire_test.c
 *
 *  Created on: 23/10/2013
 *      Author: monfee
 */

#include "atire.h"

int main(int argc, char **argv) {
	if (argc == 1)
		run_atire("-?");
	return run_atire(argv[1]);
}
