#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sched.h>
#include <pthread.h>

/* include zicio library */
#include <libzicio.h>

#define MAX_NUM_FD 64
#define __ZICIO_LIB_CHUNK_SIZE_SHIFT (21)
#define __ZICIO_LIB_CHUNK_SIZE	(1UL << __ZICIO_LIB_CHUNK_SIZE_SHIFT) /* 2 MiB */

struct zicio zicio_shared_pool;

unsigned long read_page_sizes[13] = 
				{512, 1024, 2048, 4096, 
				8192, 16384, 32768, 65536, 
				131072, 262144, 524288, 1048576, 
				2097152};

static inline void print_error(const char* err_msg) 
{
  fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
}

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct zicio *zicio)
{
	const int ull_count_per_page = 
		(zicio->read_page_size / sizeof(unsigned long));
	unsigned long v1 = 0;
	unsigned long v2 = 0;
	unsigned long etc = 0;
	unsigned long long *arr = NULL;
	unsigned long long page_cnt = 0;

	while (1) {
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;
		for (int ull_idx = 0; ull_idx < ull_count_per_page; ull_idx++) {
			if (arr[ull_idx] == 127)
				v1++;
			else if (arr[ull_idx] == 126)
				v2++;
			else
				etc++;
		}

		zicio_put_page(zicio);
		page_cnt++;

		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
			break;
	}

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value,
	 * there is error.
	 */
	return 0;
}

void do_init_shared_zicio_args(struct zicio *zicio, int *fds, int nr_fd,
			zicio_shared_pool_key_t zicio_shared_pool_key)
{
	zicio->shareable_fds = fds;
	zicio->local_fds = NULL;
	zicio->nr_shareable_fd = nr_fd;
	zicio->nr_local_fd = 0;
	zicio->zicio_flag = 0;
	zicio->zicio_shared_pool_key = zicio_shared_pool_key;
}

/* 
 * Custom data ingestion test - read 2 GiB with variable read_page_size
 */
int main(int argc, char *args[])
{
	int fds[MAX_NUM_FD];
	int nr_fd;
	struct zicio zicio;
	char *data_path;
	char path[256];
	int ret = 0;
	zicio_shared_pool_key_t zicio_shared_pool_key = 0;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc != 2) {
			print_error("the number of parameter is invalid\n");
			return -1;
	}

	data_path = args[1];

	/* Open a file */
	sprintf(path, "%s/data.%d", data_path, 0);
	if ((fds[0] = open(path, O_RDONLY | O_DIRECT)) < 0) {
		print_error("open file failed\n");
	 	return -1;
	}

	nr_fd = 1;

	/* Init zicio structure */
	zicio_init(&zicio_shared_pool);

	/* Set values before call open */
	do_init_shared_zicio_args(&zicio_shared_pool, fds, nr_fd, zicio_shared_pool_key);
	/* Create zicio create pool. If succeeding shared pool,
	   then zicio_shared_pool_key is set to a valid value */
	zicio_create_pool(&zicio_shared_pool);

	for (int i = 0; i < 13; ++i) {
		/* Init zicio structure */
		zicio_init(&zicio);

		printf("Loop[%d] starts before attaching\n", i);
		/* Set values before call open */
		do_init_shared_zicio_args(&zicio, fds, nr_fd,
					zicio_shared_pool.zicio_shared_pool_key);

		/* Open zicio */
		zicio_open(&zicio);
		if (zicio.open_status != ZICIO_OPEN_SUCCESS) {
	  		print_error("zicio open fail");
			return -1;
		} else {
		    printf("Loop[%d] starts after attaching\n", i);
        }

		/* Check zicio id */
		if (zicio.zicio_id == -1) {
	  		print_error("zicio opened, but zicio id is invalid");
			return -1;
		}

		/* Check zicio channel idx used for user */
		if (zicio.zicio_channel_idx == -1) {
	  		print_error("zicio opened, but channel idx is invalid");
			return -1;
		}

		/* 
	 	 * Call zicio_get_page() repeatedly in loop 
	 	 */
		ret = do_data_ingestion(&zicio);
	    printf("Loop[%d] after ingestion\n", i);

		/* Error status */
		if (ret) {
		  return -1;
		} else {
			printf("[SUCCESS] data ingestion with read_page_size %ld\n",
				zicio.read_page_size);
		}

		/* Close zicio */
		zicio_close(&zicio);

		/* Check zicio close success */
		if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  		print_error("zicio close fail");
			return -1;
		} else {
			printf("Loop[%d] ends\n", i);
		}
	}

	assert(zicio_shared_pool.zicio_shared_pool_key);
	if (zicio_destroy_pool(zicio_shared_pool.zicio_shared_pool_key)) {
		print_error("zicio shared pool destroy fail");
		return -1;
	}

	close(fds[0]);

	if (ret == 0)
	  return 0;
	else
	  return -1;
}
