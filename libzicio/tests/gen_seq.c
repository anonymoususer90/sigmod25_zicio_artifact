#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[])
{
    FILE* f = NULL;
	int nrGB = 0;
	int nrFiles;
	char datapath[128];
	unsigned long long counter = 0;
	unsigned long long *buffer;
	const unsigned long default_buffer_count = 128 * 1024 * 1024;
	const unsigned long default_buffer_size = 8 * default_buffer_count;

	if (argc != 4) {
		printf("./gen_seq [num of GiB] [num of files] [data path]\n");
		return -1;
	}

	nrGB = strtol(argv[1], NULL, 10);
	nrFiles = strtol(argv[2], NULL, 10);

	/* 1GB buffer */
	buffer = (unsigned long long *) malloc(default_buffer_size);
	if (buffer == NULL) {
		printf("malloc fail\n");
		return -1;
	}

	for (int file_idx = 0; file_idx < nrFiles; file_idx ++) {
		/* Open file */
		sprintf(datapath, "%s/data_seq.%d", argv[3], file_idx);
		f = fopen(datapath, "wb");
		if (f == NULL) {
			printf("fopen fail\n");
			return -1;
		} 

		/* Write file */
		for (int i = 0; i < nrGB; i++) {
			/* each gigabyte buffer has own number */
			for (unsigned long j = 0; j < default_buffer_count; j++) {
				buffer[j] = counter;
				counter += sizeof(unsigned long long);
			}

			/* write it */
			fwrite(buffer, 8, default_buffer_count, f);
		}
		fclose(f);
	}

	free(buffer);
	return 0;
}
