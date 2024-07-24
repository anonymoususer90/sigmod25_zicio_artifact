#include <stdio.h>
#include <stdlib.h>

#include <libzicio.h>

static inline void print_msg(const char *msg)
{
	fprintf(stdout, "%s\n", msg);
}

static inline void print_error(const char *err_msg)
{
	fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
}

int main(int argc, char * argv[])
{
	long ret;
	sf_shared_pool_key_t shared_pool_key;
	if (argc > 2) {
		print_error("Invalid arguments");
		print_msg("[USAGE] a.out {[shared pool key]}");
	}

	if (argc == 1) {
		ret = zicio_destroy_pool(0);
	} else {
		shared_pool_key = atoi(argv[1]);
		ret = zicio_destroy_pool(shared_pool_key);
	}

	if (ret < 0) {
		print_error("zicio shared pool destroy fail");
		return -1;
	}

	return 0;
}
