#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    FILE* f = NULL;
	int nrKB = 0;
	unsigned long long *buffer;
	const unsigned long default_buffer_count = 128;
	const unsigned long default_buffer_size = 8 * default_buffer_count;

	if (argc != 2) {
		printf("What KiB is needed?\n");
		return -1;
	}

	/* Open file */
	f = fopen("data", "wb");
	if (f == NULL) {
		printf("fopen fail\n");
		return -1;
	} 

	/* 1 KiB buffer */
	buffer = (unsigned long long *) malloc(default_buffer_size);
	if (buffer == NULL) {
		printf("malloc fail\n");
		return -1;
	}

	/* Write file */
	nrKB = strtol(argv[1], NULL, 10);
	for (int i = 0; i < nrKB; i++) {
		/* each gigabyte buffer has own number */
		for (unsigned long j = 0; j < default_buffer_count; j++) {
			buffer[j] = 127 - i;
		}

		/* write it */
		fwrite(buffer, 8, default_buffer_count, f);
	}
	fclose(f);
	free(buffer);
	return 0;
}
