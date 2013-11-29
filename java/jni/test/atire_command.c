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
	const char *result = 0;

	while (get_line(">", buffer) == OK) {
		if (strlen(buffer) > 0) {
			printf("\n");
			printf("cmd: %s\n", buffer);


			if (strcmp(buffer, "exit") == 0)
				break;
			else if (strncmp(buffer, "search ", 7) == 0)
				result = client.search(buffer + 7, 3, 1);
			else if (strncmp(buffer, "getdoc ", 7) == 0) {
				long long len;
				result = client.get_document(atol(buffer + 7), &len);
			}
			else if (strncmp(buffer, "cmd ", 4) == 0)
				result = client.send_command(buffer + 4);
			else {
				result = 0;
				printf("Invalid comand: %s\n", buffer);
			}

			if (result != 0 && strlen(result) > 0)
				printf("Result:\n%s", result);
		}
	}
	delete [] buffer;
	return 0;
}



