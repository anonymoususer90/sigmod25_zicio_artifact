#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif	/* _GNU_SOURCE */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/unistd.h>
#include <linux/zicio.h>
#include <sys/mman.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <stdint.h>
#include <sched.h>

int main(int argc, char *argv[])
{
	int desc_id = atoi(argv[1]);
	syscall(552, desc_id);
	return 0;
}
