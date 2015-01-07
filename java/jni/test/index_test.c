#include "index.h"

#include "disk.h"
#include "indexer.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
	if (argc == 1)
		atire_index("-?");

	char *text = ANT_disk::read_entire_file(argv[1]);
	char *start, *end = text;

	ATIRE_indexer indexer;

	indexer.init("-Rt[FOLDER:YMD]+kt");
	int i = 0;
	char id[1024];

	while ((start = strstr(end, "<DOC>")) != NULL) {
		end = strstr(start, "</DOC>");

		if (end != NULL) {
			static char doc[1024*2];
			int length = end + 6 - start + 1;
			strncpy(doc, start, length);
			doc[length] = '\0';
			sprintf(id, "%d", i);
			indexer.index_document(id, doc);
		}
		else {
			fputs("ill-formed xml text", stderr);
			exit(-1);
		}
		++i;
	}

	indexer.finish();

	return -1; //atire_index(argv[1]);
}
