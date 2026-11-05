/*
 * C
 *
 * Copyright 2025-2026 MicroEJ Corp. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be found with this software.
 */

#ifndef  FS_FILEX_CONFIGURATION_H
#define  FS_FILEX_CONFIGURATION_H

/**
 * @file
 * @brief FS Helper configuration for FileX .
 * @author MicroEJ Developer Team
 * @version 1.1.0
 */

// -----------------------------------------------------------------------------
// Includes
// -----------------------------------------------------------------------------

#include "fx_api.h"

// include MicroEJ User configuration file
#include <sni.h>
#if !defined(__CC_ARM)
// cppcheck-suppress [misra-c2012-20.9]: The macro '__has_include' is defined by the toolchain.
   #if __has_include("veeport_configuration.h")
	  #include "veeport_configuration.h"
   #else
	  #warning "'veeport_configuration.h' not found, default configuration used for all parameters."
   #endif // __has_include("veeport_configuration.h")
#else
   #warning "This C module needs a 'veeport_configuration.h' in your ARMCC project. "
   #include "veeport_configuration.h"
#endif // !defined ( __CC_ARM)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Use this macro to define a function that returns to point FileX to the root media.
 * This function is required.
 *
 */
#ifndef LLFS_FILEX_GET_ROOT_MEDIA_NAME
#error \
	No root media defined: please implement your root media accessor function and set 'LLFS_FILEX_GET_ROOT_MEDIA_NAME' in 'veeport_configuration.h'
#else
FX_MEDIA_PTR LLFS_FILEX_GET_ROOT_MEDIA_NAME(void);
#endif

/** @brief Wrapper macro to apply before the FileX operations */
#ifndef PRE_FILEX_OPERATION
#define PRE_FILEX_OPERATION() ((void)0)
#else
void PRE_FILEX_OPERATION(void);
#endif

/** @brief Wrapper macro to apply after the FileX operations */
#ifndef POST_FILEX_OPERATION
#define POST_FILEX_OPERATION() ((void)0)
#else
void POST_FILEX_OPERATION(void);
#endif

#ifdef __cplusplus
}
#endif

#endif // FS_FILEX_CONFIGURATION_H
