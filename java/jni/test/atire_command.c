/*
 * atire_command.c
 *
 *  Created on: 29/11/2013
 *      Author: monfee
 */

#include "../../../atire/atire_api_remote.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define OK       0
#define NONE 1
#define OVERFLOW 2

#define BUFFER_SIZE	10*1024*1024

int get_line (char *prompt, char *buffer);


 int get_line (char *prompt, char *buffer) {
    int ch, extra;

    if (prompt != NULL) {
        printf ("%s", prompt);
        fflush (stdout);
    }

    if (fgets (buffer, BUFFER_SIZE, stdin) == NULL)
        return NONE;

    if (buffer[strlen(buffer)-1] != '\n') {
        extra = 0;
        while (((ch = getchar()) != '\n') && (ch != EOF))
            extra = 1;
        return (extra == 1) ? OVERFLOW : OK;
    }

    buffer[strlen(buffer)-1] = '\0';
    return OK;
}

int main(int argc, char **argv) {
	char *buffer = new char[BUFFER_SIZE];
	ATIRE_API_remote client;
	client.open("localhost:8088");

	while (get_line(">", buffer) == OK) {
		if (strlen(buffer) > 0) {
			printf("\n");
			printf("cmd: %s", buffer);

			if (strcmp(buffer, "exit") == 0)
				break;
			else if (strncmp(buffer, "search ", 7) == 0)
				client.search(buffer + 7, 3, 1);
			else if (strncmp(buffer, "getdoc ", 7) == 0) {
				long long len;
				client.get_document(atol(buffer + 7), &len);
			}
			else
				client.send_command(buffer);
		}
	}
	delete [] buffer;
	return 0;
}



