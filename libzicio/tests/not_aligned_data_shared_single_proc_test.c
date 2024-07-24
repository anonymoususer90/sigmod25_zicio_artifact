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

#define __KiB (1024)
#define __MiB (1024 * 1024)

#define NUM_READ_PAGE_SIZE 13

struct zicio zicio_shared_pool;

unsigned long read_page_sizes[NUM_READ_PAGE_SIZE] = 
				{512, 1024, 2048, 4096, 
				8192, 16384, 32768, 65536, 
				131072, 262144, 524288, 1048576, 
				2097152};

/* Byte unit */
unsigned long data_size = 13 * 32 * __KiB + 16 * __MiB; /* 17203200 bytes */

static inline void print_error(const char* err_msg) 
{
	fprintf(stderr, "[ERROR] %s at %s:%d\n", err_msg, __FILE__, __LINE__);
}

static unsigned long
get_expected_total_page_num(int fd, unsigned long read_page_size)
{
	unsigned long file_size;
	unsigned long expected_page_num;

	file_size = lseek(fd, 0, SEEK_END);

	expected_page_num = file_size / read_page_size;

	if (file_size % read_page_size)
		expected_page_num += 1;

	return expected_page_num;
}

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct zicio *zicio, unsigned long expected)
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
	if (page_cnt != expected) {
		printf("[FAIL] read page cnt %llu, expected %lu\n", 
			page_cnt, expected);
		return -1;
	}
	else {
		printf("[SUCCESS] read page cnt %llu, expected %lu\n", 
			page_cnt, expected);
		return 0;
	}
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
 * Not aligend data ingestion test - read variable size of data 
 */
int main(int argc, char *args[])
{
	int fds[MAX_NUM_FD];
	int nr_fd;
	struct zicio zicio;
	char *data_path;
	char path[256];
	unsigned long expected_total_page_num;
	int ret = 0;
	zicio_shared_pool_key_t zicio_shared_pool_key = 0;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc != 2) {
			print_error("the number of parameter is invalid\n");
			return -1;
	}

	data_path = args[1];

	/* Open a file */
	sprintf(path, "%s/not_aligned_data.%d", data_path, 0);
	if ((fds[0] = open(path, O_RDONLY | O_DIRECT)) < 0) {
		print_error("open file failed\n");
	 	return -1;
	}

	nr_fd = 1;

	/* Init zicio structure */
	zicio_init(&zicio_shared_pool);

	/* Set values before call open */
	do_init_shared_zicio_args(&zicio_shared_pool, fds, nr_fd, zicio_shared_pool_key);

	/* Create zicio shared pool. If succeeding shared pool,
	 * then zicio_shared_pool_key is set to a valid value */
	zicio_create_pool(&zicio_shared_pool);

	if (zicio_shared_pool.open_status != ZICIO_OPEN_SUCCESS) {
		print_error("zicio shared pull open fail");
		return -1;
	}

	for (int i = 0; i < NUM_READ_PAGE_SIZE; ++i) {
		/* Init zicio structure */
		zicio_init(&zicio);

		/* Set values before call open */
		do_init_shared_zicio_args(&zicio, fds, nr_fd,
					zicio_shared_pool.zicio_shared_pool_key);

		/* Get expected total number of page */
		expected_total_page_num = 
			get_expected_total_page_num(fds[0], read_page_sizes[i]);

		/* Open zicio channel and attach it to shared pool */
		zicio_open(&zicio);
		if (zicio.open_status != ZICIO_OPEN_SUCCESS) {
	  		print_error("zicio open fail");
			return -1;
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
		ret = do_data_ingestion(&zicio, expected_total_page_num);

		/* Error status */
		if (ret) {
		  return -1;
		}

		/* Close zicio and detach it */
		zicio_close(&zicio);

		/* Check zicio close success */
		if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  		print_error("zicio close fail");
			return -1;
		}

	}

	/* Destroy zicio create pool anc check */
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
