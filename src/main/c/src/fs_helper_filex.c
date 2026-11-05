/*
 * C
 *
 * Copyright 2024-2026 MicroEJ Corp. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be found with this software.
 * Build: 7E4D1F7C.
 */

/**
 * @file
 * @brief FileX helper for LLFS.
 * @author MicroEJ Developer Team
 * @version 1.1.0
 */

// -----------------------------------------------------------------------------
// Includes
// -----------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "fx_api.h"
#include "microej_async_worker.h"
#include "microej_pool.h"
#include "osal.h"

#include "LLFS_impl.h"
#include "LLFS_File_impl.h"
#include "fs_configuration.h"
#include "fs_helper.h"
#include "fs_filex_configuration.h"

// -----------------------------------------------------------------------------
// Macros and Defines
// -----------------------------------------------------------------------------

/** @brief define the amount of file in private pool module */
#ifndef FS_MAX_NUMBER_OF_FILE_IN_POOL
#define FS_MAX_NUMBER_OF_FILE_IN_POOL  (5u)
#endif

/** @brief define the amount of dir in private pool module */
#define FS_MAX_WRITE_PADDING_SIZE_BYTES  (64u)

/*
 * The FileX return codes and attributes in the UINT type of the FileX API values they are compared with:
 * fx_api.h defines them as plain integer constants.
 */
#define FS_FX_SUCCESS          ((UINT)FX_SUCCESS)
#define FS_FX_NOT_FOUND        ((UINT)FX_NOT_FOUND)
#define FS_FX_END_OF_FILE      ((UINT)FX_END_OF_FILE)
#define FS_FX_NO_MORE_SPACE    ((UINT)FX_NO_MORE_SPACE)
#define FS_FX_ALREADY_CREATED  ((UINT)FX_ALREADY_CREATED)
#define FS_FX_NOT_DIRECTORY    ((UINT)FX_NOT_DIRECTORY)
#define FS_FX_READ_ONLY        ((UINT)FX_READ_ONLY)
#define FS_FX_HIDDEN           ((UINT)FX_HIDDEN)

/** @brief Identifier of a pool entry that holds no open file. Never given to the caller of the open action. */
#define FS_NO_FILE_ID     (0)

/** @brief First identifier given to an open file. */
#define FS_FIRST_FILE_ID  (1u)

// -----------------------------------------------------------------------------
// Typedefs and structures
// -----------------------------------------------------------------------------

/**
 * @brief Entry of the private file pool: a FileX file and the identifier given to the caller of the open action.
 *
 * The caller receives this identifier instead of the address of the FX_FILE. A closed stream can still send its old
 * identifier, and the pool gives a released entry to the next open. An address would then designate the other file.
 * An identifier is never given twice, so a stale one designates no entry.
 */
typedef struct {
	FX_FILE file;    /**< FileX file object. */
	int32_t file_id; /**< Identifier of the open file, FS_NO_FILE_ID when the entry is free. */
} FS_pool_file_t;

// -----------------------------------------------------------------------------
// Extern Variables
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Global Variables
// -----------------------------------------------------------------------------

/** @brief private pool file */
// cppcheck-suppress [misra-c2012-8.9]: False positive, this object is used in gst_pool_file_ctx that is used in many
// functions.
static FS_pool_file_t gpst_pool_file[FS_MAX_NUMBER_OF_FILE_IN_POOL];
// cppcheck-suppress [misra-c2012-8.9]: False positive, this object is used in gst_pool_file_ctx that is used in many
// functions.
static POOL_item_status_t gpst_pool_file_item_status[FS_MAX_NUMBER_OF_FILE_IN_POOL];
static POOL_ctx_t gst_pool_file_ctx =
{
	gpst_pool_file,
	gpst_pool_file_item_status,
	sizeof(FS_pool_file_t),
	sizeof(gpst_pool_file) / sizeof(FS_pool_file_t)
};

/**
 * @brief Serializes the access to the private pool file entries and to the file identifier counter.
 *
 * The pool test-and-set is not atomic, and two threads use it: the FS async worker (Java LLFS files) and the VEE task
 * (External Resources Loader, which calls the file actions directly). Without the mutex, both can get the same
 * FX_FILE.
 */
static OSAL_mutex_handle_t gst_pool_file_mutex;

/**
 * @brief Boolean to check if the search should start with the first entry in the open directory.
 */
static bool is_first_entry_to_read = false;

#ifndef FX_NO_LOCAL_PATH
/**
 * @brief < FileX local path structure corresponding to current opened directory.
 */
// cppcheck-suppress [misra-c2012-8.9]: this object is used in several functions only when traces are enabled
static FX_LOCAL_PATH opened_localpath;
#endif

// -----------------------------------------------------------------------------
// fs_helper private function prototypes
// -----------------------------------------------------------------------------

/**
 * @brief Writes dummy bytes at the current file pointer location to increase the file size.
 *
 * @param fd
 *            file identifier pointer.
 * @param size
 *            the amount of dummy bytes to write in the file.
 *
 * @return FS_FX_SUCCESS for a successful padding write or any other FileX API return value sent by fx_file_write if an
 * error occurs.
 *
 * @note Does not set the file pointer to the end of the file by itself.
 */
static UINT padding_write(FX_FILE *fd, size_t size);

/**
 * @brief Retireves the parent directory of a given file absolute path.
 *
 * @param full_path
 *            absolute path of the file that we want to get the parent directory.
 * @param parent_dir_path (output)
 *            parent directory of the file mentioned in full_path.
 * @param size
 *            size of the char array parent_dir_path.
 */
static void get_parent_directory(const char *full_path, char *parent_dir_path, size_t size);

/**
 * @brief Reserves an entry of the private pool and gives it a new file identifier, under gst_pool_file_mutex.
 *
 * @param pool_file (output)
 *            the reserved entry.
 *
 * @return POOL_NO_ERROR on success, POOL_NO_SPACE_AVAILABLE when the pool is full or when all the positive 32-bit
 * identifiers were given, or POOL_ERROR_IN_ENTRY_PARAMETERS when the mutex cannot be taken.
 */
static POOL_status_t reserve_pool_file(FS_pool_file_t **pool_file);

/**
 * @brief Clears the file identifier of an entry and frees the entry to the private pool, under gst_pool_file_mutex.
 *
 * @param pool_file
 *            the entry given by reserve_pool_file().
 */
static void free_pool_file(FS_pool_file_t *pool_file);

/**
 * @brief Tells whether an entry of the private pool has a given file identifier.
 *
 * Compare function given to POOL_get_f().
 *
 * @param item
 *            the pool entry.
 * @param characteristic
 *            pointer to the file identifier.
 *
 * @return true when the entry has the file identifier, false otherwise.
 */
static bool has_file_id(void *item, void *characteristic);

/**
 * @brief Gets the open entry of the private pool that has a file identifier, under gst_pool_file_mutex.
 *
 * @param file_id
 *            the file identifier sent by the caller of the action.
 *
 * @return the entry, or NULL when the identifier is unknown or designates a closed file.
 */
static FS_pool_file_t *get_open_pool_file(int32_t file_id);

/**
 * @brief Flushes the FileX cache of the root media to the media.
 *
 * A failure is traced only: the operation that requested the flush has already completed in the cache.
 */
static void flush_media(void);

/**
 * @brief Empties a file before it is opened in write mode: deletes it, then creates it again.
 *
 * A failure is traced only: the open that follows reports the file as missing or keeps its content.
 *
 * @param path
 *            absolute path of the file.
 */
static void recreate_file(CHAR *path);

// -----------------------------------------------------------------------------
// fs_helper private function implementation
// -----------------------------------------------------------------------------

static void flush_media(void) {
	UINT fx_status = fx_media_flush(LLFS_FILEX_GET_ROOT_MEDIA_NAME());
	if (FS_FX_SUCCESS != fx_status) {
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] media flush failed (err 0x%x)\n", __func__, __LINE__, fx_status);
	}
}

static void recreate_file(CHAR *path) {
	UINT fx_status = fx_file_delete(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
	if ((FS_FX_SUCCESS != fx_status) && (FS_FX_NOT_FOUND != fx_status)) {
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] delete of %s failed (err 0x%x)\n", __func__, __LINE__, path, fx_status);
	}
	flush_media();
	fx_status = fx_file_create(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
	if (FS_FX_SUCCESS != fx_status) {
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] create of %s failed (err 0x%x)\n", __func__, __LINE__, path, fx_status);
	}
}

static UINT padding_write(FX_FILE *fd, size_t size) {
	CHAR padding[FS_MAX_WRITE_PADDING_SIZE_BYTES] = { 0 };
	int32_t nb_loops = size / FS_MAX_WRITE_PADDING_SIZE_BYTES;
	int32_t last_bytes = size % FS_MAX_WRITE_PADDING_SIZE_BYTES;
	UINT fx_status = FS_FX_SUCCESS;

	PRE_FILEX_OPERATION();
	for (int32_t i = 0; i < nb_loops; i++) {
		fx_status = fx_file_write(fd, (void *)padding, FS_MAX_WRITE_PADDING_SIZE_BYTES);
		if (FS_FX_SUCCESS != fx_status) {
			// Error detected, we leave the loop.
			break;
		}
	}

	if ((FS_FX_SUCCESS == fx_status) && (0 < last_bytes)) {
		fx_status = fx_file_write(fd, (void *)padding, last_bytes);
	}
	POST_FILEX_OPERATION();

	return fx_status;
}

static void get_parent_directory(const char *full_path, char *parent_dir_path, size_t size) {
	const char *last_slash = NULL;
	// Find the last occurrence of '/'
	last_slash = strrchr(full_path, '/');

	// Check if '/' is found
	if (last_slash != NULL) {
		// cppcheck-suppress [misra-c2012-18.4]: use the operator - between two pointers that points to the same array.
		size_t parent_length = last_slash - full_path;

		// Ensure that the size doesn't exceed the buffer
		if (parent_length < size) {
			// Copy the parent directory part into parent_dir_path
			// cppcheck-suppress [misra-c2012-17.7]: This return value does not require checking.
			strncpy(parent_dir_path, full_path, parent_length);
			parent_dir_path[parent_length] = '\0'; // Null-terminate the string
		} else {
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("The buffer size is too small for the parent directory.\n");
		}
	} else {
		// If there's no '/', it means there is no parent directory (e.g., root file)
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("No parent directory found in the path.\n");
	}
}

static POOL_status_t reserve_pool_file(FS_pool_file_t **pool_file) {
	/* Identifier to give to the next open file. fs_helper_init() does not reset it: a stream opened before a second
	 * initialization can still send its identifier. */
	static uint32_t next_file_id = FS_FIRST_FILE_ID;
	POOL_status_t pool_res = POOL_ERROR_IN_ENTRY_PARAMETERS;

	if (OSAL_OK == OSAL_mutex_take(&gst_pool_file_mutex, OSAL_INFINITE_TIME)) {
		pool_res = POOL_reserve_f(&gst_pool_file_ctx, (void **)pool_file);
		if (POOL_NO_ERROR == pool_res) {
			if (next_file_id <= (uint32_t)INT32_MAX) {
				(*pool_file)->file_id = (int32_t)next_file_id;
				next_file_id++;
			} else {
				// All the identifiers were given: giving one again would let a stale identifier match.
				pool_res = POOL_free_f(&gst_pool_file_ctx, (void *)*pool_file);
				if (POOL_NO_ERROR != pool_res) {
					// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not
					// necessary.
					LLFS_DEBUG_TRACE("[%s:%u] POOL_free_f failed (err %d)\n", __func__, __LINE__, pool_res);
				}
				pool_res = POOL_NO_SPACE_AVAILABLE;
			}
		}
		if (OSAL_OK != OSAL_mutex_give(&gst_pool_file_mutex)) {
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] pool mutex give failed\n", __func__, __LINE__);
		}
	} else {
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] pool mutex take failed\n", __func__, __LINE__);
	}

	return pool_res;
}

static void free_pool_file(FS_pool_file_t *pool_file) {
	if (OSAL_OK == OSAL_mutex_take(&gst_pool_file_mutex, OSAL_INFINITE_TIME)) {
		pool_file->file_id = FS_NO_FILE_ID;
		POOL_status_t pool_res = POOL_free_f(&gst_pool_file_ctx, (void *)pool_file);
		if (POOL_NO_ERROR != pool_res) {
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] POOL_free_f failed (err %d)\n", __func__, __LINE__, pool_res);
		}
		if (OSAL_OK != OSAL_mutex_give(&gst_pool_file_mutex)) {
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] pool mutex give failed\n", __func__, __LINE__);
		}
	} else {
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] pool mutex take failed\n", __func__, __LINE__);
	}
}

static bool has_file_id(void *item, void *characteristic) {
	const FS_pool_file_t *pool_file = (const FS_pool_file_t *)item;
	const int32_t *file_id = (const int32_t *)characteristic;

	return (*file_id == pool_file->file_id);
}

static FS_pool_file_t *get_open_pool_file(int32_t file_id) {
	FS_pool_file_t *pool_file = NULL;
	int32_t searched_file_id = file_id;

	// FS_NO_FILE_ID and the negative values, -1 of a closed Java stream included, are never given by the open action.
	if (FS_NO_FILE_ID < file_id) {
		if (OSAL_OK == OSAL_mutex_take(&gst_pool_file_mutex, OSAL_INFINITE_TIME)) {
			POOL_status_t pool_res = POOL_get_f(&gst_pool_file_ctx, (void **)&pool_file, has_file_id,
			                                    (void *)&searched_file_id);
			// FileX sets fx_file_id while the file is open: the entry of a file that failed to open does not match.
			if ((POOL_NO_ERROR != pool_res) || (FX_FILE_ID != pool_file->file.fx_file_id)) {
				pool_file = NULL;
			}
			if (OSAL_OK != OSAL_mutex_give(&gst_pool_file_mutex)) {
				// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
				LLFS_DEBUG_TRACE("[%s:%u] pool mutex give failed\n", __func__, __LINE__);
			}
		} else {
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] pool mutex take failed\n", __func__, __LINE__);
		}
	}

	if (NULL == pool_file) {
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] invalid or closed file identifier %ld\n", __func__, __LINE__, file_id);
	}

	return pool_file;
}

// -----------------------------------------------------------------------------
// fs_helper function implementation
// -----------------------------------------------------------------------------

void fs_helper_init(void) {
	/* Creates gst_pool_file_mutex once only: fs_helper_init() runs at boot and again from LLFS_IMPL_initialize(). */
	static bool is_pool_file_mutex_created = false;

	/* Create the pool mutex on the first call only: the second call must not re-create a mutex that is in use. */
	if (false == is_pool_file_mutex_created) {
		// cppcheck-suppress [misra-c2012-11.8]: String casts conform to OSAL_mutex_create function definition.
		if (OSAL_OK == OSAL_mutex_create((uint8_t *)"MicroEJ FS pool", &gst_pool_file_mutex)) {
			is_pool_file_mutex_created = true;
		} else {
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] pool mutex creation failed\n", __func__, __LINE__);
		}
	}

	/* init private pool */
	// cppcheck-suppress [misra-c2012-17.7]: This return value does not require checking.
	memset(gpst_pool_file, 0, sizeof(gpst_pool_file));
	// cppcheck-suppress [misra-c2012-17.7]: This return value does not require checking.
	memset(gpst_pool_file_item_status, POOL_FREE, sizeof(gpst_pool_file_item_status));
}

void LLFS_IMPL_get_last_modified_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_last_modified_t *param = (FS_last_modified_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	uint8_t *path = (uint8_t *)&param->path;
	LLFS_date_t *out_date = &param->date;

	UINT attributes;
	UINT year;
	UINT month;
	UINT day;
	UINT hour;
	UINT minute;
	UINT second;
	ULONG size;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_information_get(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes, &size,
	                                         &year, &month, &day,
	                                         &hour, &minute, &second);
	POST_FILEX_OPERATION();

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		out_date->second = (int32_t)(second);
		out_date->minute = (int32_t)(minute);
		out_date->hour = (int32_t)(hour);
		out_date->day = (int32_t)(day);
		out_date->month = (int32_t)(month);
		out_date->year = (int32_t)(year);
		param->result = LLFS_OK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] timestamp get : %ld/%02ld/%02ld, %02ld:%02ld:%02ld\n (err 0x%x)\n", __func__, __LINE__,
	                 out_date->year, out_date->month, out_date->day, out_date->hour, out_date->minute, out_date->second,
	                 fx_status);
}

void LLFS_IMPL_set_read_only_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	uint8_t *path = (uint8_t *)&param->path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path);

	if (fx_status == FS_FX_NOT_DIRECTORY) {
		// Set a file as read only
		fx_status = fx_file_attributes_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, FS_FX_READ_ONLY);
	} else if (FS_FX_SUCCESS == fx_status) {
		// Set a directory as read only
		fx_status = fx_directory_attributes_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, FS_FX_READ_ONLY);
	} else {
		// Nothing to do.
	}

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		param->result = LLFS_OK;
	}

	flush_media();
	POST_FILEX_OPERATION();

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] readonly set on %s (err 0x%x)\n", __func__, __LINE__, path, fx_status);
}

void LLFS_IMPL_rename_to_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_rename_to_t *param = (FS_rename_to_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	uint8_t *path = (uint8_t *)&param->path;
	uint8_t *new_path = (uint8_t *)&param->new_path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path);

	if (fx_status == FS_FX_NOT_DIRECTORY) {
		// Rename a file
		fx_status = fx_file_rename(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, (char *)new_path);
	} else if (FS_FX_SUCCESS == fx_status) {
		// Rename a directory
		fx_status = fx_directory_rename(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, (char *)new_path);
	} else {
		// Nothing to do.
	}
	POST_FILEX_OPERATION();

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		param->result = LLFS_OK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] rename : old name %s, new name %s (err 0x%x)\n", __func__, __LINE__, path, new_path,
	                 fx_status);
}

void LLFS_IMPL_get_length_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path64_operation_t *param = (FS_path64_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	uint8_t *path = (uint8_t *)&param->path;

	UINT attributes;
	UINT year;
	UINT month;
	UINT day;
	UINT hour;
	UINT minute;
	UINT second;
	ULONG size;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_information_get(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes, &size,
	                                         &year, &month, &day, &hour, &minute, &second);
	POST_FILEX_OPERATION();

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		param->result = (int64_t)size;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] length of %s : %lld bytes (err 0x%x)\n", __func__, __LINE__, path, param->result,
	                 fx_status);
}

void LLFS_IMPL_get_space_size_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_get_space_size_t *param = (FS_get_space_size_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;
	ULONG available_bytes;

	int32_t space_type = param->space_type;

#ifdef LLFS_DEBUG
	uint8_t *path = (uint8_t *)&param->path;
#endif

	PRE_FILEX_OPERATION();
	fx_status = fx_media_space_available(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), &available_bytes);
	POST_FILEX_OPERATION();

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		switch (space_type) {
		case LLFS_USABLE_SPACE:     // Information not available, send value equivalent the result of LLFS_FREE_SPACE.
		case LLFS_FREE_SPACE:
			param->result = available_bytes;
			break;

		case LLFS_TOTAL_SPACE:
			param->result = (LLFS_FILEX_GET_ROOT_MEDIA_NAME()->fx_media_total_clusters *
			                 LLFS_FILEX_GET_ROOT_MEDIA_NAME()->fx_media_total_sectors);
			break;

		default:
			param->result = LLFS_NOK;
			break;
		}
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] get space type %ld on %s : %lld bytes (err 0x%x)\n", __func__, __LINE__, space_type, path,
	                 param->result, fx_status);
}

void LLFS_IMPL_is_hidden_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;
	UINT attributes;

	uint8_t *path = (uint8_t *)&param->path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path);

	if (fx_status == FS_FX_NOT_DIRECTORY) {
		// Get file attributes
		fx_status = fx_file_attributes_read(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes);
	} else if (FS_FX_SUCCESS == fx_status) {
		// Get directory attributes
		fx_status = fx_directory_attributes_read(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes);
	} else {
		// Nothing to do.
	}
	POST_FILEX_OPERATION();

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		if (0U != (attributes & FS_FX_HIDDEN)) {
			param->result = LLFS_OK;
		} else {
			param->result = LLFS_NOK;
		}
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] %s is hidden ? (err 0x%x)\n", __func__, __LINE__, path, fx_status);
}

void LLFS_IMPL_set_last_modified_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_last_modified_t *param = (FS_last_modified_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	uint8_t *path = (uint8_t *)&param->path;
	const LLFS_date_t *new_date = &param->date;

	PRE_FILEX_OPERATION();
	fx_status = fx_file_date_time_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, new_date->year, new_date->month,
	                                  new_date->day,
	                                  new_date->hour, new_date->minute, new_date->second);
	POST_FILEX_OPERATION();

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		param->result = LLFS_OK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] timestamp set : %ld/%02ld/%02ld, %02ld:%02ld:%02ld\n (err 0x%x)\n", __func__, __LINE__,
	                 new_date->year, new_date->month, new_date->day, new_date->hour, new_date->minute, new_date->second,
	                 fx_status);
}

void LLFS_IMPL_is_accessible_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_is_accessible_t *param = (FS_is_accessible_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;
	UINT attributes;

	uint8_t *path = (uint8_t *)&param->path;
	int32_t checked_access = param->access;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path);

	if (fx_status == FS_FX_NOT_DIRECTORY) {
		// Get file attributes
		fx_status = fx_file_attributes_read(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes);
	} else if (FS_FX_SUCCESS == fx_status) {
		// Get directory attributes
		fx_status = fx_directory_attributes_read(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes);
	} else {
		// Nothing to do.
	}
	POST_FILEX_OPERATION();

	if (FS_FX_SUCCESS != fx_status) {
		param->result = LLFS_NOK;
	} else {
		switch (checked_access) {
		case LLFS_ACCESS_WRITE:
			if (FS_FX_READ_ONLY == (attributes & FS_FX_READ_ONLY)) {
				param->result = LLFS_NOK;
			} else {
				param->result = LLFS_OK;
			}
			break;

		// FileX doesn't support other permissions so return always ok.
		case LLFS_ACCESS_READ:
			param->result = LLFS_OK;
			break;

		case LLFS_ACCESS_EXECUTE:
			param->result = LLFS_OK;
			break;

		default:
			param->result = LLFS_NOK;
			break;
		}
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] %s is accessible ? access %ld (err 0x%x)\n", __func__, __LINE__, path, checked_access,
	                 fx_status);
}

void LLFS_IMPL_set_permission_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_set_permission_t *param = (FS_set_permission_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;
	UINT attributes;
	UINT new_attributes;

	uint8_t *path = (uint8_t *)&param->path;
	int32_t access = param->access;
	int32_t enable = param->enable;

	/*
	 * FileX does not identify the owner and doesn't support other permissions other than
	 * write access (read only or not).
	 */
	switch (access) {
	case LLFS_ACCESS_WRITE:

		PRE_FILEX_OPERATION();
		fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path);

		if (fx_status == FS_FX_NOT_DIRECTORY) {
			// Get file attributes
			fx_status = fx_file_attributes_read(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes);

			if (FS_FX_SUCCESS == fx_status) {
				new_attributes = attributes & (enable ? FS_FX_READ_ONLY : 0U);
				fx_status = fx_file_attributes_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, new_attributes);
			}
		} else {
			// Get directory attributes
			fx_status = fx_directory_attributes_read(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, &attributes);

			if (FS_FX_SUCCESS == fx_status) {
				new_attributes = attributes & (enable ? FS_FX_READ_ONLY : 0U);

				fx_status = fx_directory_attributes_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path, new_attributes);
			}
		}
		POST_FILEX_OPERATION();

		if (FS_FX_SUCCESS != fx_status) {
			param->result = LLFS_NOK;
		} else {
			param->result = LLFS_OK;
		}
		break;

	// FileX doesn't support other permissions so we return always ok.
	case LLFS_ACCESS_READ:
		param->result = LLFS_OK;
		break;

	case LLFS_ACCESS_EXECUTE:
		param->result = LLFS_OK;
		break;

	default:
		param->result = LLFS_NOK;
		break;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] set permission %ld for %s as %ld (err 0x%x)\n", __func__, __LINE__, access, path, enable,
	                 fx_status);
}

void LLFS_File_IMPL_get_file_pointer_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_getfp_t *param = (FS_getfp_t *)job->params;
	const FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);

	if (NULL == pool_file) {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	} else {
		param->result = pool_file->file.fx_file_current_file_offset;
		if (param->result < 0) {
			param->error_code = LLFS_CAUGHT_EXCEPTION_SEEK;
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] negative offset %lld in FX_FILE structure\n", __func__, __LINE__, param->result);
			param->result = LLFS_NOK;
		}
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] get file pointer file %ld (position %lld)\n", __func__, __LINE__, param->file_id,
	                 param->result);
}

void LLFS_File_IMPL_set_length_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_set_length_t *param = (FS_set_length_t *)job->params;
	FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);

	if (NULL == pool_file) {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	} else {
		UINT fx_status = FS_FX_SUCCESS;
		FX_FILE *fd = &pool_file->file;
		ULONG64 old_size = fd->fx_file_current_file_size;
		ULONG64 old_pos = fd->fx_file_current_file_offset;
		ULONG64 new_size = param->length;

		PRE_FILEX_OPERATION();
		if (new_size < old_size) {
			fx_status = fx_file_truncate(fd, new_size);

			if (FS_FX_SUCCESS != fx_status) {
				if (old_pos > new_size) {
					// We update the pointer position to the end of the file after truncation if necessary.
					fx_status = fx_file_seek(fd, fd->fx_file_current_file_size);
				}
			}
		} else if (new_size > old_size) {
			/**
			 * fx_file_allocate does not increase the size of the file explicitely, it allocates clusters
			 * in order to make faster the next write operations.
			 * The size of a file is increased only with write operations. Thus, padding writing is necessary
			 * to set the file size to the new_size.
			 */
			fx_status = fx_file_allocate(fd, (new_size - old_size));
			flush_media();

			if (FS_FX_SUCCESS == fx_status) {
				// Set the file pointer to the end of the file.
				fx_status = fx_file_relative_seek(fd, 0, FX_SEEK_END);
			}
			if (FS_FX_SUCCESS == fx_status) {
				// Padding writing to reach the desired file size.
				fx_status = padding_write(fd, (new_size - old_size));
			}
			if (FS_FX_SUCCESS == fx_status) {
				// Set the file pointer to its original position.
				fx_status = fx_file_seek(fd, old_pos);
			}
		} else {
			// Nothing to do.
		}

		if (FS_FX_SUCCESS != fx_status) {
			param->result = LLFS_NOK;
			param->error_code = LLFS_CAUGHT_EXCEPTION_TRUNCATE;
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] set length failed (err 0x%x)\n", __func__, __LINE__, fx_status);
		} else {
			param->result = LLFS_OK;
		}

		flush_media();
		POST_FILEX_OPERATION();

#ifdef LLFS_DEBUG
		ULONG64 file_size = fd->fx_file_current_file_size; // Retrieve the file's size after the operation.
#endif
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] set length to %lld on %ld ; size=%lld (status %d ; err 0x%x)\n", __func__,
		                 __LINE__, new_size, param->file_id, file_size, param->result, fx_status);
	}
}

void LLFS_File_IMPL_get_length_with_fd_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_get_length_with_fd_t *param = (FS_get_length_with_fd_t *)job->params;
	const FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);

	if (NULL != pool_file) {
		param->result = pool_file->file.fx_file_current_file_size;
	} else {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] get length with fd on %ld length= %lld \n", __func__, __LINE__, param->file_id,
	                 param->result);
}

void LLFS_File_IMPL_available_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_available_t *param = (FS_available_t *)job->params;
	const FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);
	ULONG current_size;
	ULONG current_pos;

	if (NULL != pool_file) {
		current_size = pool_file->file.fx_file_current_file_size;
		current_pos = pool_file->file.fx_file_current_file_offset;

		if (current_pos > current_size) {
			param->result = 0;
		} else {
			param->result = current_size - current_pos;
		}
	} else {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] available %ld bytes on %ld \n", __func__, __LINE__, param->result, param->file_id);
}

void LLFS_File_IMPL_seek_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_seek_t *param = (FS_seek_t *)job->params;
	FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);
	UINT fx_status = FS_FX_SUCCESS;

	if (NULL == pool_file) {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	} else if (param->n < 0) {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] negative seek position %lld\n", __func__, __LINE__, param->n);
	} else {
		// The 64-bit seek keeps a position above 4 GiB: FileX stops the file pointer at the end of the file.
		PRE_FILEX_OPERATION();
		fx_status = fx_file_extended_seek(&pool_file->file, (ULONG64)param->n);
		POST_FILEX_OPERATION();

		if (FS_FX_SUCCESS != fx_status) {
			param->result = LLFS_NOK;
			param->error_code = LLFS_CAUGHT_EXCEPTION_SEEK;
		}
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] seek to %lld on %ld (status %ld ; err 0x%x)\n", __func__, __LINE__, param->n,
	                 param->file_id, param->result, fx_status);
}

void LLFS_File_IMPL_read_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_write_read_t *param = (FS_write_read_t *)job->params;
	FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);
	UINT fx_status = FS_FX_SUCCESS;
	ULONG bytesread;
	uint8_t *data = param->data;
	ULONG length = param->length;

	if (NULL != pool_file) {
		PRE_FILEX_OPERATION();
		fx_status = fx_file_read(&pool_file->file, data, length, &bytesread);
		POST_FILEX_OPERATION();

		if (FS_FX_SUCCESS != fx_status) {
			if (FS_FX_END_OF_FILE == fx_status) {
				param->result = LLFS_EOF;
			} else {
				param->result = LLFS_NOK;
				param->error_code = LLFS_CAUGHT_EXCEPTION_READ;
			}
		} else {
			param->result = (int32_t)bytesread;
		}
	} else {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] read %ld bytes from file %ld (err 0x%x)\n", __func__, __LINE__, param->result,
	                 param->file_id, fx_status);
}

void LLFS_File_IMPL_open_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	int32_t result = LLFS_OK;
	FS_open_t *param = (FS_open_t *)job->params;
	FS_pool_file_t *pool_file = NULL;
	FX_FILE *fp = NULL;
	UINT fx_status = FS_FX_SUCCESS;
	POOL_status_t pool_res;
	UINT internal_mode = FX_NULL;

	CHAR *path = (CHAR *)&param->path;
	CHAR mode = param->mode;

	// Map input mode to FileX mode
	switch (mode) {
	case LLFS_FILE_MODE_APPEND:
		internal_mode = FX_OPEN_FOR_WRITE;
		break;

	case LLFS_FILE_MODE_READ:
		internal_mode = FX_OPEN_FOR_READ;
		break;

	case LLFS_FILE_MODE_WRITE:
		internal_mode = FX_OPEN_FOR_WRITE;
		break;

	case LLFS_FILE_MODE_READ_WRITE:
	case LLFS_FILE_MODE_READ_WRITE_DATA_SYNC:
	case LLFS_FILE_MODE_READ_WRITE_SYNC:
		internal_mode = FX_OPEN_FOR_WRITE;
		break;

	default:
		result = LLFS_NOK;
		param->result = result;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] invalid opening mode %c\n", __func__, __LINE__, mode);
		break;
	}

	if (result == LLFS_OK) {
		PRE_FILEX_OPERATION();
		// Empty the file in write mode, but not in append mode.
		if (mode == LLFS_FILE_MODE_WRITE) {
			recreate_file(path);
		}

		pool_res = reserve_pool_file(&pool_file);
		if (pool_res != POOL_NO_ERROR) {
			param->result = LLFS_NOK;
			param->error_code = LLFS_CAUGHT_EXCEPTION_RESOURCE_EXHAUSTED;
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] reserve_pool_file failed (err %d)\n", __func__, __LINE__, pool_res);
		} else {
			fp = &pool_file->file;
			fx_status = fx_file_open(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), fp, path, internal_mode);

			// Create the file if it does not exist and if we are not in read mode.
			if ((FS_FX_NOT_FOUND == fx_status) && (LLFS_FILE_MODE_READ != mode)) {
				fx_status = fx_file_create(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
				if (FS_FX_SUCCESS == fx_status) {
					fx_status = fx_file_open(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), fp, path, internal_mode);
				}
			}

			if (fx_status != FS_FX_SUCCESS) {
				free_pool_file(pool_file);
				param->result = LLFS_NOK;
				param->error_code = LLFS_CAUGHT_EXCEPTION_OPEN;
			} else {
				// seek at the end of file if append mode or at the start (in case of write close and rewrite)
				fx_status = fx_file_seek(fp,
				                         (mode == LLFS_FILE_MODE_APPEND) ? (ULONG)fp->fx_file_current_file_size : (ULONG)0U);

				if (fx_status != FS_FX_SUCCESS) {
					param->result = LLFS_NOK;
					param->error_code = LLFS_CAUGHT_EXCEPTION_SEEK;
					// The file must leave the open file list of FileX before another open gets its pool entry.
					UINT close_status = fx_file_close(fp);
					if (FS_FX_SUCCESS != close_status) {
						// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not
						// necessary.
						LLFS_DEBUG_TRACE("[%s:%u] close after seek failure failed (err 0x%x)\n", __func__, __LINE__,
						                 close_status);
					}
					free_pool_file(pool_file);
				} else {
					param->result = pool_file->file_id;
				}
			}
		}
		POST_FILEX_OPERATION();
	}
	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] open file %s in %c mode, result %ld (err 0x%x)\n", __func__, __LINE__, path, mode,
	                 param->result, fx_status);
}

void LLFS_File_IMPL_close_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_close_t *param = (FS_close_t *)job->params;
	FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);
	UINT fx_status = FS_FX_SUCCESS;

	if (NULL == pool_file) {
		// A second close of the same file ends here: its entry is not freed twice.
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	} else {
		PRE_FILEX_OPERATION();
		fx_status = fx_file_close(&pool_file->file);

		if (fx_status != FS_FX_SUCCESS) {
			param->result = LLFS_NOK;
			param->error_code = LLFS_CAUGHT_EXCEPTION_CLOSE;
		} else {
			param->result = LLFS_OK;
		}

		free_pool_file(pool_file);

		flush_media();
		POST_FILEX_OPERATION();
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] close file %ld (status %ld err 0x%x)\n", __func__, __LINE__, param->file_id,
	                 param->result, fx_status);
}

void LLFS_IMPL_create_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_create_t *param = (FS_create_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;
	UINT attributes = 0;
	CHAR *path = (CHAR *)&param->path;
	CHAR parent_dir[FX_MAX_LONG_NAME_LEN];

	// Check if we have the permission in the parent directory.
	get_parent_directory(path, parent_dir, sizeof(parent_dir));

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_attributes_read(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)parent_dir, &attributes);

	if (FS_FX_SUCCESS != fx_status) {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_STAT;
		// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
		LLFS_DEBUG_TRACE("[%s:%u] attribute read of the parent directory failed (err 0x%x)\n", __func__, __LINE__,
		                 fx_status);
	} else {
		if (FS_FX_READ_ONLY == (attributes & FS_FX_READ_ONLY)) {
			param->result = LLFS_NOK;
			param->error_code = LLFS_CAUGHT_EXCEPTION_PERMISSION;
			// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
			LLFS_DEBUG_TRACE("[%s:%u] cannot write in the parent directory\n", __func__, __LINE__);
		} else {
			fx_status = fx_file_create(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);

			if (fx_status == FS_FX_SUCCESS) {
				param->result = LLFS_OK;
			} else if (fx_status == FS_FX_ALREADY_CREATED) {
				param->result = LLFS_NOT_CREATED;
				param->error_code = LLFS_CAUGHT_EXCEPTION_UNDEFINED;
				// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
				LLFS_DEBUG_TRACE("[%s:%u] the file already exists\n", __func__, __LINE__);
			} else {
				param->result = LLFS_NOT_CREATED;
				param->error_code = LLFS_CAUGHT_EXCEPTION_UNDEFINED;
				// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
				LLFS_DEBUG_TRACE("[%s:%u] fx_file_create failed (err 0x%x)\n", __func__, __LINE__, fx_status);
			}
		}
	}

	flush_media();
	POST_FILEX_OPERATION();

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] create file %s (err 0x%x)\n", __func__, __LINE__, path, fx_status);
}

void LLFS_IMPL_delete_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;
	CHAR type[16] = { 0 };
	CHAR *path = (CHAR *)&param->path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);

	// The tested path is a file if FS_FX_NOT_DIRECTORY is returned.
	if (FS_FX_NOT_DIRECTORY == fx_status) {
		// cppcheck-suppress [misra-c2012-17.7]: This return value does not require checking.
		strcpy(type, "file");
		PRE_FILEX_OPERATION();
		fx_status = fx_file_delete(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
		POST_FILEX_OPERATION();
	} else if (FS_FX_SUCCESS == fx_status) {
		// cppcheck-suppress [misra-c2012-17.7]: This return value does not require checking.
		strcpy(type, "directory");
		PRE_FILEX_OPERATION();
		fx_status = fx_directory_delete(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
		POST_FILEX_OPERATION();
	} else {
		// Nothing to do.
	}

	if (FS_FX_SUCCESS != fx_status) {
		param->result = LLFS_NOK;
	} else {
		param->result = LLFS_OK;
	}

	flush_media();
	POST_FILEX_OPERATION();

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] delete the %s %s (err 0x%x)\n", __func__, __LINE__, type, path, fx_status);
}

void LLFS_IMPL_make_directory_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	uint8_t *path = (uint8_t *)&param->path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_create(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), (char *)path);

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
	} else {
		param->result = LLFS_OK;
	}

	flush_media();
	POST_FILEX_OPERATION();

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] create dir %s (err 0x%x)\n", __func__, __LINE__, path, fx_status);
}

void LLFS_IMPL_open_directory_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
#ifdef LLFS_DEBUG
	UINT fx_status = FS_FX_SUCCESS;
#endif
	CHAR *path = (CHAR *)&param->path;

	PRE_FILEX_OPERATION();
	// detect if path is root
	if ((strcmp(path, "/") != 0) && (fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(),
	                                                        path) != FS_FX_SUCCESS)) {
		// so path is not root and doesn't exist
		param->result = LLFS_NOK;
	} else {
#ifdef FX_NO_LOCAL_PATH
		// No local path (FileX standalone mode): list the directory through the media default path. Only
		// one directory is open at a time, and the other operations take absolute paths.
		if (FS_FX_SUCCESS != fx_directory_default_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path)) {
#else
		if (FS_FX_SUCCESS != fx_directory_local_path_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), &opened_localpath, path)) {
#endif
			param->result = LLFS_NOK;
		} else {
			param->result = LLFS_OK;
			is_first_entry_to_read = true;
		}
	}
	POST_FILEX_OPERATION();

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] Open directory %s ; ID %p (err 0x%x)\n", __func__, __LINE__, path, param->result,
	                 fx_status);
}

void LLFS_IMPL_close_directory_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_close_directory_t *param = (FS_close_directory_t *)job->params;

	PRE_FILEX_OPERATION();
#ifdef FX_NO_LOCAL_PATH
	// Set the media default path back to the root.
	if (FS_FX_SUCCESS != fx_directory_default_set(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), FX_NULL)) {
#else
	if (FS_FX_SUCCESS != fx_directory_local_path_clear(LLFS_FILEX_GET_ROOT_MEDIA_NAME())) {
#endif
		param->result = LLFS_NOK;
	} else {
		param->result = LLFS_OK;
	}
	POST_FILEX_OPERATION();

#ifdef FX_NO_LOCAL_PATH
	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] Free directory (err %d)\n", __func__, __LINE__, param->result);
#else
	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] Free directory ID %s (err %d)\n", __func__, __LINE__, opened_localpath.fx_path_string,
	                 param->result);
#endif
}

void LLFS_File_IMPL_write_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_write_read_t *param = (FS_write_read_t *)job->params;
	unsigned int byteswritten;
	UINT fx_status = FS_FX_SUCCESS;
	FS_pool_file_t *pool_file = get_open_pool_file(param->file_id);
	uint8_t *data = param->data;
	int32_t length = param->length;

	if (NULL == pool_file) {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_INVALID_PARAMETER;
	} else {
		PRE_FILEX_OPERATION();
		fx_status = fx_file_write(&pool_file->file, (void *)data, length);
		POST_FILEX_OPERATION();

		if (fx_status != FS_FX_SUCCESS) {
			param->result = LLFS_NOK;
			param->error_code =
				(FS_FX_NO_MORE_SPACE == fx_status) ? LLFS_CAUGHT_EXCEPTION_DISK_FULL : LLFS_CAUGHT_EXCEPTION_WRITE;
		} else {
			/**
			 * The FileX API does not provide the amount of bytes written,
			 * so if the operation is successful, the amount of bytes written is length.
			 */
			byteswritten = length;
			param->result = (int32_t)byteswritten;
		}
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] written %ld bytes to file %ld (err 0x%x)\n", __func__, __LINE__, param->result,
	                 param->file_id, fx_status);
}

void LLFS_IMPL_exist_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	CHAR *path = (CHAR *)&param->path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
	POST_FILEX_OPERATION();

	// test return function (ok if it' directory or file else nok)
	if ((fx_status == FS_FX_SUCCESS) || (fx_status == FS_FX_NOT_DIRECTORY)) {
		param->result = LLFS_OK;
	} else {
		param->result = LLFS_NOK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] path exist %s (err 0x%x)\n", __func__, __LINE__, path, fx_status);
}

void LLFS_IMPL_is_file_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	CHAR *path = (CHAR *)&param->path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
	POST_FILEX_OPERATION();

	// The tested path is a file if FS_FX_NOT_DIRECTORY is returned.
	if (fx_status == FS_FX_NOT_DIRECTORY) {
		param->result = LLFS_OK;
	} else {
		param->result = LLFS_NOK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] %s is file ? (result %d ; fileX err 0x%x)\n", __func__, __LINE__, path, param->result,
	                 fx_status);
}

void LLFS_IMPL_is_directory_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_path_operation_t *param = (FS_path_operation_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	CHAR *path = (CHAR *)&param->path;

	PRE_FILEX_OPERATION();
	fx_status = fx_directory_name_test(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
	POST_FILEX_OPERATION();

	// The tested path is a directory if FS_FX_SUCCESS is returned.
	if (fx_status == FS_FX_SUCCESS) {
		param->result = LLFS_OK;
	} else {
		param->result = LLFS_NOK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] %s is directory ? (err 0x%x)\n", __func__, __LINE__, path, fx_status);
}

void LLFS_IMPL_read_directory_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_read_directory_t *param = (FS_read_directory_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;
	CHAR *path = (CHAR *)&param->path;
	if (true == is_first_entry_to_read) {
		PRE_FILEX_OPERATION();
		fx_status = fx_directory_first_entry_find(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
		POST_FILEX_OPERATION();

		is_first_entry_to_read = false;
	} else {
		PRE_FILEX_OPERATION();
		fx_status = fx_directory_next_entry_find(LLFS_FILEX_GET_ROOT_MEDIA_NAME(), path);
		POST_FILEX_OPERATION();
	}
	if (fx_status != FS_FX_SUCCESS) {
		// error or no more entry in directory.
		param->result = LLFS_NOK;
		is_first_entry_to_read = true;
	} else {
		param->result = LLFS_OK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] read dir %s (err 0x%x)\n", __func__, __LINE__, path,
	                 fx_status);
}

void LLFS_File_IMPL_flush_action(MICROEJ_ASYNC_WORKER_job_t *job) {
	FS_flush_t *param = (FS_flush_t *)job->params;
	UINT fx_status = FS_FX_SUCCESS;

	PRE_FILEX_OPERATION();
	fx_status = fx_media_flush(LLFS_FILEX_GET_ROOT_MEDIA_NAME());
	POST_FILEX_OPERATION();

	if (fx_status != FS_FX_SUCCESS) {
		param->result = LLFS_NOK;
		param->error_code = LLFS_CAUGHT_EXCEPTION_SYNC;
	} else {
		param->result = LLFS_OK;
	}

	// cppcheck-suppress [misra-c2012-17.7]: The use of returned values of debug traces is not necessary.
	LLFS_DEBUG_TRACE("[%s:%u] flush fileX cache (status %ld err 0x%x)\n", __func__, __LINE__, param->result, fx_status);
}
