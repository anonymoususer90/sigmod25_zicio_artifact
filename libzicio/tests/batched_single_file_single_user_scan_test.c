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

#define MAX_NUM_FD	(64)
#define FILE_SHIFT	(31)
#define FILE_SIZE	(5 * ((unsigned long long)1 << FILE_SHIFT)) // 10 GiB

#define PAGE_SHIFT	(13)
#define PAGE_SIZE	(1 << PAGE_SHIFT) // 8 KiB

#define PAGES_PER_FILE	((unsigned long long)(FILE_SIZE) / \
						 (unsigned long long)(PAGE_SIZE))

#define print_error(err_msg) \
	print_error_internal(err_msg, __FILE__, __LINE__)

unsigned long long *page_nums;

static inline void print_error_internal(const char* err_msg,
										char *file, int lineno) 
{
	fprintf(stderr, "[ERROR] \"%s\" at %s:%d\n", err_msg, file, lineno);
}

static inline void set_pages(struct zicio *zicio, int fd, unsigned long start,
							  unsigned long end, int *nr_page,
							  unsigned long long *page_nums) {
	zicio_notify_ranges(zicio, fd, start, end);
	for (unsigned long long page = start; page <= end; page++)
		page_nums[(*nr_page)++] = page;
}

/*
 * do_data_ingestion
 *
 * Return 0, success
 * Return -1, error
 */
static int do_data_ingestion(struct zicio *zicio, int nr_page,
							 unsigned long long *page_nums)
{
	const int ull_count_per_page = 
		(PAGE_SIZE / sizeof(unsigned long));
	unsigned long long *arr = NULL;
	unsigned long long page_num;
	int cnt = 0;

	while (1) {
		while (zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
			zicio_get_page(zicio);
		}

		assert(zicio->page_addr != NULL);
		arr = (unsigned long long *)zicio->page_addr;

		for (int ull_idx = 0; ull_idx < ull_count_per_page; ull_idx++) {
			page_num = arr[ull_idx] >> PAGE_SHIFT;
			if (page_num != page_nums[cnt]) {
				print_error("mismatched page_num");
				fprintf(stderr, "expected value: %llu, page_num: %llu, ull_idx: %d\n", page_nums[cnt], page_num, ull_idx);
				return -1;
			}

			if (arr[ull_idx] % PAGE_SIZE != ull_idx * sizeof(unsigned long long)) {
				print_error("mismatched value");
				fprintf(stderr, "expected value: %ld, arr[ull_idx]: %llu, ull_idx: %d\n", ull_idx * sizeof(unsigned long long), arr[ull_idx] % PAGE_SIZE, ull_idx);
				return -1;
			}
		}
		cnt++;

		zicio_put_page(zicio);
		if (zicio->put_status == ZICIO_PUT_PAGE_EOF)
			break;
	}

	/* 
	 * If there are other values(i.e. etc) or v1 and v2 have different value or
	 * cnt is different with nr_page, there is error.
	 */
	if (cnt != nr_page)
		return -1;
	else
		return 0;
}

/* 
 * Batched data ingestion test - read 10 GiB from one file
 */
int main(int argc, char *args[])
{
	int fds[MAX_NUM_FD];
	int fd;
	struct zicio zicio;
	char *data_path;
	char path[256];
	int ret = 0;
	int nr_page = 0;

	/* $FILE_PATH/$FILE_NAME $DATA_FILE_PATH */
	if (argc != 2) {
			print_error("the number of parameter is invalid");
			return -1;
	}

	/* Init zicio structure */
	zicio_init(&zicio);

	/* Set values before call open */
	zicio.local_fds = fds;
	zicio.shareable_fds = NULL;
	zicio.nr_shareable_fd = 0;
	zicio.zicio_flag = 0;
	zicio.read_page_size = PAGE_SIZE;

	data_path = args[1];

	/* Open a file */
	sprintf(path, "%s/data_seq.%d", data_path, 0);

	if ((fd = open(path, O_RDONLY | O_DIRECT)) < 0) {
		print_error("open file failed");
		return -1;
	}

	page_nums =
		(unsigned long long*)malloc(PAGES_PER_FILE * sizeof(unsigned long long));

	set_pages(&zicio, fd, 0, PAGES_PER_FILE - 1, &nr_page, page_nums);

	/* Open zicio */
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
	ret = do_data_ingestion(&zicio, nr_page, page_nums);

	/* Close zicio */
	zicio_close(&zicio);

	/* Check zicio close success */
	if (zicio.close_status != ZICIO_CLOSE_SUCCESS) {
	  	print_error("zicio close fail");
		return -1;
	}

	free(page_nums);
	close(fds[0]);

	if (ret == 0)
	  return 0;
	else
	  return -1;
}
