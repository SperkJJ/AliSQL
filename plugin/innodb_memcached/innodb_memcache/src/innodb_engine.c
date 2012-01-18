/***********************************************************************

Copyright (c) 2011, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
59 Temple Place, Suite 330, Boston, MA 02111-1307 USA

***********************************************************************/

#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "default_engine.h"
#include <memcached/util.h>
#include <memcached/config_parser.h>

#include "innodb_engine.h"
#include "innodb_engine_private.h"
#include "innodb_api.h"
#include "hash_item_util.h"

/* Static and local to this file */
const char * set_ops[] = { "","add","set","replace","append","prepend","cas" };

static inline
struct innodb_engine*
innodb_handle(
/*==========*/
	ENGINE_HANDLE*	handle) 
{
	return (struct innodb_engine*) handle;
}

static inline
struct default_engine*
default_handle(
/*===========*/
	struct innodb_engine*	eng) 
{
	return (struct default_engine*) eng->m_default_engine;
}

/****** Gateway to the default_engine's create_instance() function */
ENGINE_ERROR_CODE
create_my_default_instance(
/*=======================*/
	uint64_t,
	GET_SERVER_API,
	ENGINE_HANDLE **);

/*********** FUNCTIONS IMPLEMENTING THE PUBLISHED API BEGIN HERE ********/

ENGINE_ERROR_CODE
create_instance(
/*============*/
	uint64_t		interface, 
	GET_SERVER_API		get_server_api,
	ENGINE_HANDLE**		handle )
{
	ENGINE_ERROR_CODE	e;
	struct innodb_engine*	innodb_eng;

	SERVER_HANDLE_V1 *api = get_server_api();

	if (interface != 1 || api == NULL) {
		return ENGINE_ENOTSUP;
	}

	innodb_eng = malloc(sizeof(struct innodb_engine)); 
	if(innodb_eng == NULL) {
		return ENGINE_ENOMEM;
	}

	innodb_eng->engine.interface.interface = 1;
	innodb_eng->engine.get_info        = innodb_get_info;
	innodb_eng->engine.initialize      = innodb_initialize;
	innodb_eng->engine.destroy         = innodb_destroy;
	innodb_eng->engine.allocate        = innodb_allocate;
	innodb_eng->engine.remove          = innodb_remove;
	innodb_eng->engine.release         = innodb_release;
	innodb_eng->engine.get             = innodb_get;
	innodb_eng->engine.get_stats       = innodb_get_stats;
	innodb_eng->engine.reset_stats     = innodb_reset_stats;
	innodb_eng->engine.store           = innodb_store;
	innodb_eng->engine.arithmetic      = innodb_arithmetic;
	innodb_eng->engine.flush           = innodb_flush;
	innodb_eng->engine.unknown_command = innodb_unknown_command;
	innodb_eng->engine.item_set_cas    = item_set_cas;
	innodb_eng->engine.get_item_info   = innodb_get_item_info; 
	innodb_eng->engine.get_stats_struct = NULL;
	innodb_eng->engine.errinfo = NULL;

	innodb_eng->server = *api;
	innodb_eng->get_server_api = get_server_api;

	/* configuration, with default values*/
	innodb_eng->info.info.description = "InnoDB Memcache " VERSION;
	innodb_eng->info.info.num_features = 3;
	innodb_eng->info.info.features[0].feature = ENGINE_FEATURE_CAS;
	innodb_eng->info.info.features[1].feature = ENGINE_FEATURE_PERSISTENT_STORAGE;
	innodb_eng->info.info.features[0].feature = ENGINE_FEATURE_LRU;

	/* Now call create_instace() for the default engine */
	e = create_my_default_instance(interface, get_server_api, 
				 &(innodb_eng->m_default_engine));
	if(e != ENGINE_SUCCESS) return e;

	innodb_eng->initialized = true;

	*handle = (ENGINE_HANDLE*) &innodb_eng->engine;

	return ENGINE_SUCCESS;
}


/*** get_info ***/
static
const engine_info*
innodb_get_info(
/*============*/
	ENGINE_HANDLE*	handle)
{
	return & innodb_handle(handle)->info.info;
}

typedef struct eng_config_info {
	char*		option_string;
	void*		cb_ptr;
	unsigned int	eng_r_batch_size;
	unsigned int	eng_w_batch_size;
	bool		enable_binlog;
} eng_config_info_t;

/*** initialize ***/
static
ENGINE_ERROR_CODE
innodb_initialize(
/*==============*/
	ENGINE_HANDLE*	handle,
	const char*	config_str) 
{   
	ENGINE_ERROR_CODE	return_status = ENGINE_SUCCESS;
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	struct default_engine*	def_eng = default_handle(innodb_eng);
	eng_config_info_t*	my_eng_config;

	my_eng_config = (eng_config_info_t*) config_str;

	/* Register the call back function */
	register_innodb_cb((void*) my_eng_config->cb_ptr);

	innodb_eng->r_batch_size = (my_eng_config->eng_r_batch_size
					? my_eng_config->eng_r_batch_size
					: CONN_NUM_READ_COMMIT);
	innodb_eng->w_batch_size = (my_eng_config->eng_w_batch_size
					? my_eng_config->eng_w_batch_size
					: CONN_NUM_WRITE_COMMIT);

	innodb_eng->enable_binlog = my_eng_config->enable_binlog;

	/* If binlog is not enabled by InnoDB memcached plugin, let's
	check whether innodb_direct_access_enable_binlog is turned on */
	if (!innodb_eng->enable_binlog) {
		innodb_eng->enable_binlog = innodb_cb_binlog_enabled();
	}

	/* MEMCACHED_RESOLVE: Set the default write batch size to 1
	if binlog is turned on */
	if (innodb_eng->enable_binlog) {
		innodb_eng->w_batch_size = (innodb_eng->w_batch_size == 32)
						? 1
						: innodb_eng->w_batch_size;
	}

	UT_LIST_INIT(innodb_eng->conn_data);
	pthread_mutex_init(&innodb_eng->conn_mutex, NULL);

	/* Fetch InnoDB specific settings */
	if (!innodb_config(&innodb_eng->meta_info)) {
		return(ENGINE_FAILED);
	}

	if (innodb_eng->m_default_engine) {
		return_status = def_eng->engine.initialize(
			innodb_eng->m_default_engine,
			my_eng_config->option_string);
	}

	return(return_status);
}

extern void handler_close_thd(void*);

/* Cleanup a connection
@return number of connection cleaned */
static
int
innodb_conn_clean(
/*==============*/
	innodb_engine_t*	engine,		/*!< in/out: InnoDB memcached
						engine */
	bool			clear_all,	/*!< in: Clear all connection */
	bool			has_lock)	/*!< in: Has engine mutext */
{
	innodb_conn_data_t*	conn_data;
	innodb_conn_data_t*	next_conn_data;
	int			num_freed = 0;

	LOCK_CONN_IF_NOT_LOCKED(has_lock, engine);
	conn_data = UT_LIST_GET_FIRST(engine->conn_data);

	while (conn_data) {
		bool	stale_data = FALSE;
		void*	cookie = conn_data->c_cookie;
		next_conn_data = UT_LIST_GET_NEXT(c_list, conn_data);

		if (!clear_all && !conn_data->c_in_use) {
			innodb_conn_data_t*	check_data;
			check_data = engine->server.cookie->get_engine_specific(
				cookie);

			/* The check data is the original conn_data stored
			in connection "cookie", it can be set to NULL if
			connection closed, or to a new conn_data if it is
			closed and reopened. So verify and see if our
			current conn_data is stale */
			if (!check_data || check_data != conn_data) {
				stale_data = TRUE;
			}
		}

		/* Either we are clearing all conn_data or this conn_data is
		not in use */
		if (clear_all || stale_data) {
			MCI_LIST_REMOVE(c_list, engine->conn_data, conn_data);

			if (conn_data->c_idx_crsr) {
				innodb_cb_cursor_close(conn_data->c_idx_crsr);
			}

			if (conn_data->c_r_idx_crsr) {
				innodb_cb_cursor_close(conn_data->c_r_idx_crsr);
			}

			if (conn_data->c_crsr) {
				innodb_cb_cursor_close(conn_data->c_crsr);
			}

			if (conn_data->c_r_crsr) {
				innodb_cb_cursor_close(conn_data->c_r_crsr);
			}

			if (conn_data->c_r_trx) {
				innodb_cb_trx_commit(conn_data->c_r_trx);
			}

			if (conn_data->c_trx) {
				innodb_cb_trx_commit(conn_data->c_trx);
			}

			if (conn_data->mysql_tbl) {
				assert(conn_data->thd);
				handler_unlock_table(conn_data->thd,
						     conn_data->mysql_tbl,
						     HDL_READ);
			}

			if (conn_data->thd) {
				handler_close_thd(conn_data->thd);
				conn_data->thd = NULL;
			}

			free(conn_data);

			if (clear_all) {
				engine->server.cookie->store_engine_specific(cookie, NULL);
			}

			num_freed++;
		}

		conn_data = next_conn_data;
	}

	assert(!clear_all || engine->conn_data.count == 0);

	UNLOCK_CONN_IF_NOT_LOCKED(has_lock, engine);

	return(num_freed);
}

/*** destroy ***/
static
void
innodb_destroy(
/*===========*/
	ENGINE_HANDLE*	handle,		/*!< in: Destroy the engine instance */
	bool		force)		/*!< in: Force to destroy */
{
	struct innodb_engine* innodb_eng = innodb_handle(handle);
	struct default_engine *def_eng = default_handle(innodb_eng);

	innodb_conn_clean(innodb_eng, TRUE, FALSE);

	pthread_mutex_destroy(&innodb_eng->conn_mutex);

	if (innodb_eng->m_default_engine) {
		def_eng->engine.destroy(innodb_eng->m_default_engine, force);
	}

	innodb_config_free(&innodb_eng->meta_info);

	free(innodb_eng);
}


/*** allocate ***/

/* Allocate gets a struct item from the slab allocator, and fills in 
   everything but the value.  It seems like we can just pass this on to 
   the default engine; we'll intercept it later in store().
   This is also called directly from finalize_read() in the commit thread.
*/
static
ENGINE_ERROR_CODE
innodb_allocate(
/*============*/
	ENGINE_HANDLE*	handle,
	const void*	cookie,
	item **		item,
	const void*	key,
	const size_t	nkey,
	const size_t	nbytes,
	const int	flags,
	const rel_time_t exptime)
{
	struct innodb_engine* innodb_eng = innodb_handle(handle);
	struct default_engine *def_eng = default_handle(innodb_eng);

	/* We use default engine's memory allocator to allocate memory
	for item */
	return def_eng->engine.allocate(innodb_eng->m_default_engine,
					cookie, item, key, nkey, nbytes,
					flags, exptime);
}

/* Initialize a connection's cursor and transactions
@return the connection's conn_data structure */
static
innodb_conn_data_t*
innodb_conn_init(
/*=============*/
	innodb_engine_t*	engine,		/*!< in: InnoDB memcached
						engine */
	const void*		cookie,		/*!< in: This connection's
						cookie */
	bool			is_select,	/*!< in: Select only query */
	ib_lck_mode_t		lock_mode,	/*!< in: Table lock mode */
	bool			has_lock)	/*!< in: Has engine mutex */
{
	innodb_conn_data_t*	conn_data;
	meta_info_t*		meta_info = &engine->meta_info;
	meta_index_t*		meta_index = &meta_info->m_index;
	ib_err_t		err = DB_SUCCESS;

	LOCK_CONN_IF_NOT_LOCKED(has_lock, engine);

	/* Get this connection's conn_data */
	conn_data = engine->server.cookie->get_engine_specific(cookie);

	assert(!conn_data || !conn_data->c_in_use);

	if (!conn_data) {
		if (UT_LIST_GET_LEN(engine->conn_data) > 2048) {
			/* Some of conn_data can be stale, recycle them */
			innodb_conn_clean(engine, FALSE, TRUE);
		}

		conn_data = malloc(sizeof(*conn_data));
		memset(conn_data, 0, sizeof(*conn_data));
		conn_data->c_cookie = (void*) cookie;
		UT_LIST_ADD_LAST(c_list, engine->conn_data, conn_data);
		engine->server.cookie->store_engine_specific(
			cookie, conn_data);
	}

	assert(engine->conn_data.count > 0);
	conn_data->c_in_use = TRUE;

	UNLOCK_CONN_IF_NOT_LOCKED(has_lock, engine);

	/* Each connection comes with a read cursor and write cursor,
	and a read transaction and write transaction committed
	intermittently */
	if (!conn_data->c_r_trx) {
		conn_data->c_r_trx = innodb_cb_trx_begin(
			IB_TRX_READ_UNCOMMITTED);

		err = innodb_api_begin(
			engine,
			meta_info->m_item[META_DB].m_str,
			meta_info->m_item[META_TABLE].m_str,
			conn_data,
		 	conn_data->c_r_trx,
			&conn_data->c_r_crsr,
			&conn_data->c_r_idx_crsr,
			(lock_mode == IB_LOCK_X)
				? IB_LOCK_X
				: IB_LOCK_IS);

		if (err != DB_SUCCESS) {
			innodb_cb_cursor_close(conn_data->c_r_crsr);
			innodb_cb_trx_commit(conn_data->c_r_trx);
			conn_data->c_r_trx = NULL;
			conn_data->c_r_crsr = NULL;
			conn_data->c_in_use = FALSE;

			return(NULL);
		} else if (lock_mode == IB_LOCK_X) {

			/* Already hold exclusive table lock on the
			table, no need to acquire additional write
			lock */
			return(conn_data);
		}

		/* If not a read only query, initialize a write cursor */
		if (!is_select) {
			conn_data->c_trx = innodb_cb_trx_begin(
				IB_TRX_READ_UNCOMMITTED);

			err = innodb_api_begin(
				engine,
				meta_info->m_item[META_DB].m_str,
				meta_info->m_item[META_TABLE].m_str,
				conn_data,
				conn_data->c_trx, &conn_data->c_crsr,
				&conn_data->c_idx_crsr, lock_mode);

			if (err != DB_SUCCESS) {
				innodb_cb_cursor_close(conn_data->c_crsr);
				conn_data->c_crsr = NULL;
				if (conn_data->c_r_crsr) {
					innodb_cb_cursor_close(
						conn_data->c_r_crsr);
					conn_data->c_r_crsr = NULL;
					conn_data->c_r_trx = NULL;
				}
				conn_data->c_in_use = FALSE;
				return(NULL);
			}
		}
	} else {
		ib_crsr_t	crsr;
		ib_crsr_t	idx_crsr;

		crsr = conn_data->c_crsr;

		if (!is_select) {
			if (!crsr) {
				conn_data->c_trx = innodb_cb_trx_begin(
					IB_TRX_READ_UNCOMMITTED);

				err = innodb_api_begin(
					engine,
					meta_info->m_item[META_DB].m_str,
					meta_info->m_item[META_TABLE].m_str,
					conn_data,
					conn_data->c_trx,
					&conn_data->c_crsr,
					&conn_data->c_idx_crsr,
					lock_mode);

				if (err != DB_SUCCESS) {
					innodb_cb_cursor_close(
						conn_data->c_crsr);
					conn_data->c_crsr = NULL;
					conn_data->c_trx = NULL;
					conn_data->c_in_use = FALSE;
					return(NULL);
				}
			}  else if (!conn_data->c_trx) {

				/* There exists a cursor, just need update
				with a new transaction */
				conn_data->c_trx = innodb_cb_trx_begin(
					IB_TRX_READ_UNCOMMITTED);

				innodb_cb_cursor_new_trx(crsr, conn_data->c_trx);
				err = innodb_cb_cursor_lock(crsr, lock_mode);

				if (err != DB_SUCCESS) {
					innodb_cb_cursor_close(
						conn_data->c_crsr);
					conn_data->c_crsr = NULL;
					conn_data->c_trx = NULL;
					conn_data->c_in_use = FALSE;
					return(NULL);
				}

				if (meta_index->m_use_idx == META_SECONDARY) {

					idx_crsr = conn_data->c_idx_crsr;
					innodb_cb_cursor_new_trx(
						idx_crsr, conn_data->c_trx);
					innodb_cb_cursor_lock(
						idx_crsr, lock_mode);
				}
			}
		} else {
			if (!conn_data->c_r_trx) {
				conn_data->c_r_trx = innodb_cb_trx_begin(
					IB_TRX_READ_UNCOMMITTED);

				innodb_cb_cursor_new_trx(conn_data->c_r_crsr,
						     conn_data->c_r_trx);

                                innodb_cb_cursor_lock(conn_data->c_r_crsr,
						  lock_mode);

                                if (meta_index->m_use_idx == META_SECONDARY) {
                                        idx_crsr = conn_data->c_r_idx_crsr;

                                        innodb_cb_cursor_new_trx(
                                                idx_crsr, conn_data->c_r_trx);
                                        innodb_cb_cursor_lock(
						idx_crsr, lock_mode);
                                }
			}
		}
	}

	return(conn_data);
}

/*** remove ***/
static
ENGINE_ERROR_CODE
innodb_remove(
/*==========*/
	ENGINE_HANDLE*		handle,
	const void*		cookie,
	const void*		key,
	const size_t		nkey,
	uint64_t		cas,
	uint16_t		vbucket)
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	struct default_engine*	def_eng = default_handle(innodb_eng);
	ENGINE_ERROR_CODE	err = ENGINE_SUCCESS;
	innodb_conn_data_t*	conn_data;
	meta_info_t*		meta_info = &innodb_eng->meta_info;

	if (meta_info->m_set_option == META_CACHE
	    || meta_info->m_set_option == META_MIX) {
		hash_item*	item = item_get(def_eng, key, nkey);

		if (item != NULL) {
			item_unlink(def_eng, item);
			item_release(def_eng, item);
		}

		if (meta_info->m_set_option == META_CACHE) {
			return(ENGINE_SUCCESS);
		}
	}

	conn_data = innodb_conn_init(innodb_eng, cookie,
				     FALSE, IB_LOCK_IX, FALSE);

	/* In the binary protocol there is such a thing as a CAS delete.
	This is the CAS check. If we will also be deleting from the database,
	there are two possibilities:
	  1: The CAS matches; perform the delete.
	  2: The CAS doesn't match; delete the item because it's stale.
	Therefore we skip the check altogether if(do_db_delete) */

	err = innodb_api_delete(innodb_eng, conn_data, key, nkey);

	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_DELETE);

	return(err);
}


/*** release ***/
static
void
innodb_release(
/*===========*/
	ENGINE_HANDLE*		handle,
	const void*		cookie,
	item*			item)
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	struct default_engine*	def_eng = default_handle(innodb_eng);

	if (item) {
		item_release(def_eng, (hash_item *) item);
	}

	return;
}  

/*** get ***/
static
ENGINE_ERROR_CODE
innodb_get(
/*=======*/
	ENGINE_HANDLE*		handle,
	const void*		cookie,
	item**			item,
	const void*		key,
	const int		nkey,
	uint16_t		vbucket)
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	hash_item*		it = NULL;
	ib_crsr_t		crsr;
	ib_err_t		err = DB_SUCCESS;
	mci_item_t		result;
	ENGINE_ERROR_CODE	err_ret = ENGINE_SUCCESS;
	uint64_t		cas = 0;
	uint64_t		exp = 0;
	uint64_t		flags = 0;
	innodb_conn_data_t*	conn_data;
	int			total_len = 0;
	meta_info_t*		meta_info = &innodb_eng->meta_info;

	if (meta_info->m_set_option == META_CACHE
	    || meta_info->m_set_option == META_MIX) {
		*item = item_get(default_handle(innodb_eng), key, nkey);

		if (*item != NULL) {
			return(ENGINE_SUCCESS);
		}

		if (meta_info->m_set_option == META_CACHE) {
			return(ENGINE_KEY_ENOENT);
		}
	}

	conn_data = innodb_conn_init(innodb_eng, cookie, TRUE,
				     IB_LOCK_IX, FALSE);

	err = innodb_api_search(innodb_eng, conn_data, &crsr, key,
				nkey, &result, NULL, TRUE);

	if (err != DB_SUCCESS) {
		err_ret = ENGINE_KEY_ENOENT;
		goto func_exit;
	}

	/* Only if expiration field is enabled, and the value is not zero,
	we will check whether the item is expired */
	if (result.mci_item[MCI_COL_EXP].m_enabled
	    && result.mci_item[MCI_COL_EXP].m_digit) {
		uint64_t		time;
		time = mci_get_time();

		if (time > result.mci_item[MCI_COL_EXP].m_digit) {
			err_ret = ENGINE_KEY_ENOENT;
			goto func_exit;
		}
	}

	if (result.mci_item[MCI_COL_FLAG].m_enabled) {
		flags = ntohl(result.mci_item[MCI_COL_FLAG].m_digit);
	}

	if (result.mci_item[MCI_COL_CAS].m_enabled) {
		cas = result.mci_item[MCI_COL_CAS].m_digit;
	}

	if (result.mci_item[MCI_COL_EXP].m_enabled) {
		exp = result.mci_item[MCI_COL_EXP].m_digit;
	}

	if (result.mci_add_value) {
		int	i;
		for (i = 0; i < result.mci_add_num; i++) {

			if (result.mci_add_value[i].m_len == 0) {
				continue;
			}

			total_len += (result.mci_add_value[i].m_len
				      + meta_info->m_sep_len);
		}

		/* No need to add the last separator */
		total_len -= meta_info->m_sep_len;
	} else {
		total_len = result.mci_item[MCI_COL_VALUE].m_len;
	}

	innodb_allocate(handle, cookie, item, key, nkey, total_len, flags, exp);

        it = *item;

	if (it->iflag & ITEM_WITH_CAS) {
		hash_item_set_cas(it, cas);
	}

	if (result.mci_add_value) {
		int	i;
		char*	c_value = hash_item_get_data(it);

		for (i = 0; i < result.mci_add_num; i++) {

			if (result.mci_add_value[i].m_len == 0) {
				continue;
			}

			memcpy(c_value,
			       result.mci_add_value[i].m_str,
			       result.mci_add_value[i].m_len);

			c_value += result.mci_add_value[i].m_len;
			memcpy(c_value, meta_info->m_separator,
			       meta_info->m_sep_len);
			c_value += meta_info->m_sep_len;
		}
	} else {
		memcpy(hash_item_get_data(it),
		       result.mci_item[MCI_COL_VALUE].m_str, it->nbytes);

		if (result.mci_item[MCI_COL_VALUE].m_allocated) {
			free(result.mci_item[MCI_COL_VALUE].m_str);
		}
	}

func_exit:

	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_READ);

	return(err_ret);
}


/*** get_stats ***/
static
ENGINE_ERROR_CODE
innodb_get_stats(
/*=============*/
	ENGINE_HANDLE*		handle,
	const void*		cookie,
	const char*		stat_key,
	int			nkey,
	ADD_STAT		add_stat)
{
	struct innodb_engine* innodb_eng = innodb_handle(handle);
	struct default_engine *def_eng = default_handle(innodb_eng);
	return def_eng->engine.get_stats(innodb_eng->m_default_engine, cookie,
					 stat_key, nkey, add_stat);
}


/*** reset_stats ***/
static
void
innodb_reset_stats(
/*===============*/
	ENGINE_HANDLE* handle, 
	const void *cookie)
{
	struct innodb_engine* innodb_eng = innodb_handle(handle);
	struct default_engine *def_eng = default_handle(innodb_eng);
	def_eng->engine.reset_stats(innodb_eng->m_default_engine, cookie);
}


/*** store ***/
static
ENGINE_ERROR_CODE
innodb_store(
/*=========*/
	ENGINE_HANDLE*		handle,
	const void*		cookie,
	item*			item,
	uint64_t*		cas,
	ENGINE_STORE_OPERATION	op,
	uint16_t		vbucket)
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	uint16_t		len = hash_item_get_key_len(item);
	char*			value = hash_item_get_key(item);
	uint64_t		exptime = hash_item_get_exp(item);
	uint64_t		flags = hash_item_get_flag(item);
	ENGINE_ERROR_CODE	result;
	uint64_t		input_cas;
	innodb_conn_data_t*	conn_data;
	meta_info_t*		meta_info = &innodb_eng->meta_info;
	uint32_t		val_len = ((hash_item*)item)->nbytes;

	if (meta_info->m_set_option == META_CACHE
	    || meta_info->m_set_option == META_MIX) {
		result = store_item(default_handle(innodb_eng), item, cas,
				    op, cookie);

		if (meta_info->m_set_option == META_CACHE) {
			return(result);
		}
	}

	conn_data = innodb_conn_init(innodb_eng, cookie, FALSE,
				     IB_LOCK_IX, FALSE);

	input_cas = hash_item_get_cas(item);

	result = innodb_api_store(innodb_eng, conn_data, value, len, val_len,
				  exptime, cas, input_cas, flags, op);

	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_WRITE);

	return(result);  
}


/*** arithmetic ***/
static
ENGINE_ERROR_CODE
innodb_arithmetic(
/*==============*/
	ENGINE_HANDLE*	handle,
	const void*	cookie,
	const void*	key,
	const int	nkey,
	const bool	increment,
	const bool	create,
	const uint64_t	delta,
	const uint64_t	initial,
	const rel_time_t exptime,
	uint64_t*	cas,
	uint64_t*	result,
	uint16_t	vbucket)
{
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	struct default_engine*	def_eng = default_handle(innodb_eng);
	innodb_conn_data_t*	conn_data;
	meta_info_t*		meta_info = &innodb_eng->meta_info;
	ENGINE_ERROR_CODE	err;

	if (meta_info->m_set_option == META_CACHE
	    || meta_info->m_set_option == META_MIX) {
		/* For cache-only, forward this to the
		default engine */
		err = def_eng->engine.arithmetic(
			innodb_eng->m_default_engine, cookie, key, nkey,
			increment, create, delta, initial, exptime, cas,
			result, vbucket);

		if (meta_info->m_set_option == META_CACHE) {
			return(err);
		}
	}

	conn_data = innodb_conn_init(innodb_eng, cookie, FALSE,
				     IB_LOCK_IX, FALSE);

	innodb_api_arithmetic(innodb_eng, conn_data, key, nkey, delta,
			      increment, cas, exptime, create, initial,
			      result);

	innodb_api_cursor_reset(innodb_eng, conn_data, CONN_OP_WRITE);

	return ENGINE_SUCCESS;
}


/*** flush ***/
static
ENGINE_ERROR_CODE
innodb_flush(
/*=========*/
	ENGINE_HANDLE*	handle,
	const void*	cookie,
	time_t		when)
{                                   
	struct innodb_engine*	innodb_eng = innodb_handle(handle);
	struct default_engine*	def_eng = default_handle(innodb_eng);
	ENGINE_ERROR_CODE	err = ENGINE_SUCCESS;
	meta_info_t*		meta_info = &innodb_eng->meta_info;
	ib_err_t		ib_err = DB_SUCCESS;
	innodb_conn_data_t*	conn_data;

	if (meta_info->m_set_option == META_CACHE
	    || meta_info->m_set_option == META_MIX) {
		/* default engine flush */
		err = def_eng->engine.flush(innodb_eng->m_default_engine,
					    cookie, when);

		if (meta_info->m_set_option == META_CACHE) {
			return(err);
		}
	}

        pthread_mutex_lock(&innodb_eng->conn_mutex);

	conn_data = innodb_eng->server.cookie->get_engine_specific(cookie);

	if (conn_data) {
		innodb_api_cursor_reset(innodb_eng, conn_data,
					CONN_OP_FLUSH);
	}

	innodb_conn_clean(innodb_eng, FALSE, TRUE);

        conn_data = innodb_conn_init(innodb_eng, cookie, FALSE,
				     IB_LOCK_X, TRUE);

	
	if (!conn_data) {
		pthread_mutex_unlock(&innodb_eng->conn_mutex);
		return(ENGINE_SUCCESS);
	}

	/* Clean up sessions before doing flush. Table needs to be
	re-opened */
	innodb_conn_clean(innodb_eng, TRUE, TRUE);

	ib_err = innodb_api_flush(innodb_eng,
				  meta_info->m_item[META_DB].m_str,
			          meta_info->m_item[META_TABLE].m_str);

        pthread_mutex_unlock(&innodb_eng->conn_mutex);

	
	return(ENGINE_SUCCESS);
}


/*** unknown_command ***/
static
ENGINE_ERROR_CODE
innodb_unknown_command(
/*===================*/
	ENGINE_HANDLE*	handle,
	const void*	cookie,
	protocol_binary_request_header *request,
	ADD_RESPONSE	response)
{
	struct innodb_engine* innodb_eng = innodb_handle(handle);
	struct default_engine *def_eng = default_handle(innodb_eng);

	return def_eng->engine.unknown_command(innodb_eng->m_default_engine,
					       cookie, request, response);
}


/*** get_item_info ***/
static
bool
innodb_get_item_info(
/*=================*/
	ENGINE_HANDLE*		handle, 
	const void*		cookie,
	const item* 		item, 
	item_info*		item_info)
{
	hash_item*	it;

	if (item_info->nvalue < 1) {
	return false;
	}
	/* Use a hash item */
	it = (hash_item*) item;
	item_info->cas = hash_item_get_cas(it);
	item_info->exptime = it->exptime;
	item_info->nbytes = it->nbytes;
	item_info->flags = it->flags;
	item_info->clsid = it->slabs_clsid;
	item_info->nkey = it->nkey;
	item_info->nvalue = 1;
	item_info->key = hash_item_get_key(it);
	item_info->value[0].iov_base = hash_item_get_data(it);
	item_info->value[0].iov_len = it->nbytes;    
	return true;
}


/* read_cmdline_options requires duplicating code from the default engine. 
If the default engine supports a new option, you will need to add it here.
We create a single config_items structure containing options for both 
engines. This function is not used for now. We passed the configure
string to default engine for its initialization. But we would need this
function if we would need to parse InnoDB specific configure strings.
*/
void
read_cmdline_options(
/*=================*/
	struct innodb_engine*	innodb,
	struct default_engine*	se,
	const char*		conf)
{
	int did_parse = 0;   /* 0 = success from parse_config() */
	if (conf != NULL) {
		struct config_item items[] = {
			/* DEFAULT ENGINE OPTIONS */
			{ .key = "use_cas",
			.datatype = DT_BOOL,
			.value.dt_bool = &se->config.use_cas },
			{ .key = "verbose",
			.datatype = DT_SIZE,
			.value.dt_size = &se->config.verbose },
			{ .key = "eviction",
			.datatype = DT_BOOL,
			.value.dt_bool = &se->config.evict_to_free },
			{ .key = "cache_size",
			.datatype = DT_SIZE,
			.value.dt_size = &se->config.maxbytes },
			{ .key = "preallocate",
			.datatype = DT_BOOL,
			.value.dt_bool = &se->config.preallocate },
			{ .key = "factor",
			.datatype = DT_FLOAT,
			.value.dt_float = &se->config.factor },
			{ .key = "chunk_size",
			.datatype = DT_SIZE,
			.value.dt_size = &se->config.chunk_size },
			{ .key = "item_size_max",
			.datatype = DT_SIZE,
			.value.dt_size = &se->config.item_size_max },
			{ .key = "config_file",
			.datatype = DT_CONFIGFILE },
			{ .key = NULL}
		};

		did_parse = se->server.core->parse_config(conf, items, stderr);
	}
}

