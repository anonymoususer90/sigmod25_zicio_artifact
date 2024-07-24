/* Copyright (c) 2000, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* Read through all rows sequntially */

#include "my_dbug.h"
#include "my_inttypes.h"
#include "storage/myisam/myisamdef.h"

#ifdef __ZICIO_STAT
#include <libzicio.h>
#endif /* __ZICIO_STAT */

#ifdef __ZICIO_PAGINATION
#ifdef __ZICIO_ACTIVATE
std::unordered_map<std::string, struct zicio_shared_pool_t*> zicio_shared_pool_map;
pthread_mutex_t zicio_shared_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * zicio_find_shared_pool
 * Find a zicio shared pool matched with @pool_key
 */
uint32_t zicio_find_shared_pool(char *pool_key)
{
  std::unordered_map <std::string, struct zicio_shared_pool_t *> :: iterator it;
  pthread_mutex_lock(&zicio_shared_pool_lock);
  if ((it = zicio_shared_pool_map.find(pool_key)) != zicio_shared_pool_map.end()) {
    pthread_mutex_unlock(&zicio_shared_pool_lock);
    return it->second->zicio_shared_pool.zicio_shared_pool_key;
  }
  pthread_mutex_unlock(&zicio_shared_pool_lock);
  return UINT32_MAX;
}

/*
 * zicio_find_shared_pool_no_protect
 * Find a zicio shared pool matched with @pool_key
 */
uint32_t zicio_find_shared_pool_no_protect(char *pool_key)
{
  std::unordered_map <std::string, struct zicio_shared_pool_t *> :: iterator it;
  if ((it = zicio_shared_pool_map.find(pool_key)) != zicio_shared_pool_map.end()) {
    return it->second->zicio_shared_pool.zicio_shared_pool_key;
  }
  return UINT32_MAX;
}

/*
 * zicio_mysql_file_open
 * Open a file matched with data path
 */
File zicio_mysql_file_open(const char *data_path)
{
  File file;
  MY_STAT stat_info;
  if ((file = mysql_file_open(mi_key_file_dfile, data_path, O_RDONLY,
                              MYF(MY_WME))) < 0) {
    return -1;
  }

  if (mysql_file_fstat(file, &stat_info) != 0) {
    mysql_file_close(file, MYF(MY_WME));
    return -1;
  }

  if (!MY_S_ISREG(stat_info.st_mode)) {
    mysql_file_close(file, MYF(MY_WME));
    return -1;
  }

  return file;
}

/*
 * zicio_insert_shared_pool
 * Insert and create zicio shared pool
 */
bool zicio_insert_shared_pool(char *pool_key, const char *data_path)
{
  File file;
  pthread_mutex_lock(&zicio_shared_pool_lock);
  if (zicio_find_shared_pool_no_protect(pool_key) != UINT32_MAX) {
    pthread_mutex_unlock(&zicio_shared_pool_lock);
    return false;
  }
  if ((file = zicio_mysql_file_open(data_path)) < 0) {
    pthread_mutex_unlock(&zicio_shared_pool_lock);
    return false;
  }

  struct zicio_shared_pool_t *zicio_shared_pool = new zicio_shared_pool_t(file);

  if (!zicio_shared_pool->zicio_create_shared_pool()) {
    delete zicio_shared_pool;

    pthread_mutex_unlock(&zicio_shared_pool_lock);
    return false;
  }
  zicio_shared_pool_map.insert(std::make_pair(pool_key, zicio_shared_pool));
  pthread_mutex_unlock(&zicio_shared_pool_lock);
  return true;
}

/*
 * zicio_delete_shared_pool
 * Delete and destroy zicio shared pool
 */
bool zicio_delete_shared_pool(char *pool_key)
{
  pthread_mutex_lock(&zicio_shared_pool_lock);
  if (zicio_find_shared_pool_no_protect(pool_key) == UINT32_MAX) {
    pthread_mutex_unlock(&zicio_shared_pool_lock);
    return false;
  }

  std::unordered_map <std::string, struct zicio_shared_pool_t *> :: iterator it;
  it = zicio_shared_pool_map.find(pool_key);
  struct zicio_shared_pool_t *zicio_shared_pool = it->second;

  zicio_shared_pool_map.erase(pool_key);
  pthread_mutex_unlock(&zicio_shared_pool_lock);

  zicio_shared_pool->zicio_destroy_shared_pool();
  delete zicio_shared_pool;

  return true;
}

#endif /* __ZICIO_ACTIVATE */

/*
 * For debug
 */
void print_max_offsets(MI_INFO *info) {
  uint max_offset;
  for (uint i = 0; i <= info->last_page; i++) {
    info->s->file_read(info, (uchar*)(&max_offset), 4,
                       i * __PAGE_SIZE + 4, MYF(0));
    fprintf(stderr, "%u page max_offset: %u\n", i, max_offset);
  }
}

/*
 * mi_put_page
 *
 * @info - structure holding basic information about current operation
 *
 * Return : 0 - success
 *
 * Put this page being used
 */
int mi_put_page_pread(MI_INFO *info) {
  info->cur_max_offset = 0;

  /* If current page is last, we read all pages */
  if (++info->cur_page > info->last_page)
    info->is_all_read = 1;

  return 0;
}

#ifdef __ZICIO_ACTIVATE
int mi_put_page_zicio(MI_INFO *info) {
  zicio_put_page(&(info->zicio));
  
  /* If current page is last, we read all pages */
  if (info->zicio.put_status == ZICIO_PUT_PAGE_EOF)
    info->is_all_read = 1;

  return 0;
}
#endif /* __ZICIO_ACTIVATE */

/*
 * mi_get_page
 *
 * @info - structure holding basic information about current operation
 *
 * Return : 0 - got next page
 *          1 - no more next page
 *
 * Get next page to consume
 */
int mi_get_page_pread(MI_INFO *info) {
  /* Set next page */
  if (unlikely(info->cur_page > info->last_page)) {
    info->is_scanning = 0;
    return 1;
  }

  /* Read next page */
  info->s->file_read(info, info->page, __PAGE_SIZE,
                     (info->cur_page * __PAGE_SIZE), MYF(0));

#ifdef IO_COUNT
  info->io_count++;
#endif /* IO_COUNT */

  /* Set the last offset of this page */
  if (likely(info->cur_page < info->last_page))
    info->cur_max_offset = *((uint*)(info->page + 4));
  else  
    info->cur_max_offset = __get_offset(info->state->data_file_length);
  
  return 0;
}

#ifdef __ZICIO_ACTIVATE
int mi_get_page_zicio(MI_INFO *info) {
  struct zicio *zicio = &(info->zicio);

  zicio_get_page(zicio);
  while(zicio->get_status != ZICIO_GET_PAGE_SUCCESS) {
    pthread_yield();
    zicio_get_page(zicio);
  }

  /* Set next page */
  info->page = (uchar*)(zicio->page_addr);
  info->cur_page = *((uint*)(info->page));

  /* And set the last offset of this page */
  if (likely(info->cur_page < info->last_page))
    info->cur_max_offset = *((uint*)(info->page + 4));
  else  
    info->cur_max_offset = __get_offset(info->state->data_file_length);
  
  return 0;
}
#endif /* __ZICIO_ACTIVATE */

/*
 * mi_pagination_init
 *
 * @info - structure holding basic information about current operation
 *
 * Return : 0 - success
 *          1 - fail
 *
 * Pagination for zicIO
 * To use zicIO, the data file must be paginated.
 * These functions are functions that emulate the environment using zicIO.
 */
int mi_pagination_init_pread(MI_INFO *info) {
  /* Allocate page buffer */
  info->page = (uchar*)malloc(__PAGE_SIZE);
  
  /* Init variables */
  info->cur_page = 0;
  info->last_page = __get_page_number(info->state->data_file_length);
  info->is_scanning = 1;
  info->is_all_read = 0;

  /* Read the first page */
  if (mi_get_page_pread(info))
    return 1;

  return 0;
}

#ifdef __ZICIO_ACTIVATE
int mi_pagination_init_zicio(MI_INFO *info) {
  /* Init variables */
  info->last_page = __get_page_number(info->state->data_file_length);
  info->is_scanning = 1;
  info->is_all_read = 0;

  /* Init zicIO channel */
  struct zicio *zicio = &(info->zicio);
  uint32_t zicio_shared_pool_key;
  char pool_key[FN_REFLEN] = { '\0' };
  bool is_shareable = false;
  zicio_init(zicio);
  info->zicio_channel_fds[0] = info->dfile;

  if (info->zicio_shared_pool_num != INVALID_POOL_NUMBER) {
    /* Get hash key of shared pool */
    std::sprintf(pool_key, "%s%u", info->rel_name, info->zicio_shared_pool_num);

    if ((zicio_shared_pool_key = zicio_find_shared_pool(pool_key)) != UINT32_MAX)
      is_shareable = true;
    else
      fprintf(stderr, "shared pool doesn't exist, using local channel\n");
  }

  zicio->zicio_flag = 0;
  zicio->read_page_size = __PAGE_SIZE;

  if (is_shareable) {
	  /* Create zicio channel with shared pool */
    zicio->local_fds = NULL;
    zicio->shareable_fds = info->zicio_channel_fds;
    zicio->nr_local_fd = 0;
    zicio->nr_shareable_fd = 1;
    zicio->zicio_shared_pool_key = zicio_shared_pool_key;
  } else {
    /* Create zicio channel without shared pool */
    zicio->local_fds = info->zicio_channel_fds;
    zicio->shareable_fds = NULL;
    zicio->nr_local_fd = 1;
    zicio->nr_shareable_fd = 0;
  }

  zicio_open(zicio);
  if (zicio->open_status != ZICIO_OPEN_SUCCESS)
    return 1;

  /* Check zicio id */
  if (zicio->zicio_id == -1) {
    fprintf(stderr, "zicio opened, but zicio id is invalid\n");
    return -1;
  }

  /* Check zicio channel idx used for user */
  if (zicio->zicio_channel_idx == -1) {
    fprintf(stderr, "zicio opened, but channel idx is invalid\n");
    return -1;
  }

  /* Read the first page */
  if (mi_get_page_zicio(info))
    return 1;

  return 0;
}
#endif /* __ZICIO_ACTIVATE */

/*
 * mi_pagination_free
 *
 * @info - structure holding basic information about current operation
 *
 * Return : 0 - success
 *
 * free the page buffer
 */
int mi_pagination_free_pread(MI_INFO *info) {
  free(info->page);
  info->page = nullptr;

  return 0;
}

# ifdef __ZICIO_ACTIVATE
int mi_pagination_free_zicio(MI_INFO *info) {
#ifdef __ZICIO_STAT
  char *msg = zicio_get_stat_msg(&(info->zicio));

  if (msg != NULL) {
    fprintf(stderr, "[stat] [ZICIO] query %s, %s\n",
            stopwatch.query, msg);
    free(msg);
  }
#endif /* __ZICIO_STAT */

  zicio_close(&(info->zicio));
  return 0;
}
#endif /* __ZICIO_ACTIVATE */

/*
 * mi_max_offset_init
 *
 * @info - structure holding basic information about current operation
 *
 * Return : 0 - success
 *          1 - fail
 *
 * Offset adjustment for pagination
 * Because of pagination, offset adjustment functions are needed even if
 * zicIO is not used.
 */
int mi_max_offset_init(MI_INFO *info) {
  /* Init current page number and last page number */
  info->cur_page = 0;
  info->last_page = __get_page_number(info->state->data_file_length);

  /* Read the max_offset of first page */
  if (info->s->file_read(info, (uchar*)(&(info->cur_max_offset)), 4,
                         4, MYF(0)) == 0)
    return 1;

  /* Check if this page is the last page */
  if (unlikely(info->cur_page == info->last_page))
    info->cur_max_offset = __get_offset(info->state->data_file_length);
  
  return 0;
}

/*
 * mi_set_new_max_offset
 *
 * @info - structure holding basic information about current operation
 *
 * Return : 0 - got next page's max offset
 *          1 - no more next page
 *
 * Read and set max offset of next page
 */
int mi_set_new_max_offset(MI_INFO *info) {
  /* If read all pages */
  if (++info->cur_page > info->last_page)
    return 1;
  
  /* Check if this page is the last page */
  if (likely(info->cur_page < info->last_page))
    info->s->file_read(info, (uchar*)(&(info->cur_max_offset)), 4,
                      (info->cur_page * __PAGE_SIZE) + 4, MYF(MY_NABP));
  else  
    info->cur_max_offset = __get_offset(info->state->data_file_length);

  return 0;
}
#endif /* __ZICIO_PAGINATION */

int mi_scan_init(MI_INFO *info) {
  DBUG_TRACE;

#ifdef __ZICIO_PAGINATION
#ifdef __ZICIO_ACTIVATE

  /* Init and set the starting point */
  if (likely(info->state->data_file_length >= __PAGE_SIZE) &&
      info->enable_zicio) {
    mi_pagination_init_zicio(info);
    info->mi_put_page = mi_put_page_zicio;
    info->mi_get_page = mi_get_page_zicio;
    info->mi_pagination_free = mi_pagination_free_zicio;
  }
  else {
    mi_pagination_init_pread(info);
    info->mi_put_page = mi_put_page_pread;
    info->mi_get_page = mi_get_page_pread;
    info->mi_pagination_free = mi_pagination_free_pread;
  }
#else /* !__ZICIO_ACTIVATE */
  mi_pagination_init_pread(info);
  info->mi_put_page = mi_put_page_pread;
  info->mi_get_page = mi_get_page_pread;
  info->mi_pagination_free = mi_pagination_free_pread;
#endif /* __ZICIO_ACTIVATE */
  info->nextpos = info->cur_page * __PAGE_SIZE +
                 (8 + info->s->pack.header_length);
#else /* !__ZICIO_PAGINATION */
  info->nextpos = info->s->pack.header_length; /* Read first record */
#endif /* __ZICIO_PAGINATION */
  info->lastinx = -1;                          /* Can't forward or backward */
  if (info->opt_flag & WRITE_CACHE_USED && flush_io_cache(&info->rec_cache))
    return my_errno();
  return 0;
}

/*
           Read a row based on position.
           If filepos= HA_OFFSET_ERROR then read next row
           Return values
           Returns one of following values:
           0 = Ok.
           HA_ERR_END_OF_FILE = EOF.
*/

int mi_scan(MI_INFO *info, uchar *buf) {
  int result;
  DBUG_TRACE;
  /* Init all but update-flag */
  info->update &= (HA_STATE_CHANGED | HA_STATE_ROW_CHANGED);
#ifdef __ZICIO_STAT
  if (stopwatch.enable_zicio_stat) {
    stopwatch.once_elapsed_io_time = 0;
#ifdef PREAD_BREAKDOWN
	  zicio_enable_pread_breakdown();
#endif /* PREAD_BREAKDOWN */
  }
#endif /* __ZICIO_STAT */
  result = (*info->s->read_rnd)(info, buf, info->nextpos, true);
#ifdef __ZICIO_STAT
  if (stopwatch.enable_zicio_stat) {
    stopwatch.scan_elapsed_io_time += stopwatch.once_elapsed_io_time;
#ifdef PREAD_BREAKDOWN
	  zicio_disable_pread_breakdown();
#endif /* PREAD_BREAKDOWN */
  }
#endif /* __ZICIO_STAT */
  return result;
}
