<!--
	Markdown
-->
![FS](https://shields.microej.com/endpoint?url=https://repository.microej.com/packages/badges/fs_6.json)

# Abstraction Layer File System (FS) on top of FileX

# Overview

This component implements functions in the fs_helper.h file provided by the FS Generic C module to connect to a Board Support Package using the File X file system.

See the MicroEJ documentation for a description of the `LLFS` functions:
- [LLFS: File System](https://docs.microej.com/en/latest/VEEPortingGuide/appendix/llapi.html#llfs-file-system)
- [File System: Installation](https://docs.microej.com/en/latest/VEEPortingGuide/fs.html#installation)
- [File System](https://docs.microej.com/en/latest/ApplicationDeveloperGuide/libraries.html#libraries)

This implementation has a default configuration file, [fs_filex_configuration.h](src/main/c/inc/fs_filex_configuration.h). This component support user configuration parameters in the `veeport_configuration.h` file.

# Usage

1. These sources can be included in the VEE Port with the method you prefer, by using this repository as a submodule or by doing a copy of the sources in the VEE Port repository.
2. Implement your root media accessor function and set `LLFS_FILEX_GET_ROOT_MEDIA_NAME` in a `veeport_configuration.h` file. If necessary, implement the functions that must run before and/or after the FileX API calls and set the corresponding values of the PRE_FILEX_OPERATION and POST_FILEX_OPERATION defines in the `veeport_configuration.h` file.
3. The configuration file [fs_filex_configuration.h](src/main/c/inc/fs_filex_configuration.h) provides the default values of the helper configuration. If you want to update a configuration please edit or create the file `veeport_configuration.h` and set the desired value. This setting overwrites the content of `fs_filex_configuration.h.`

# Requirements

N/A

# Validation

This Abstraction Layer implementation can be validated in the target Board Support Package using the FS Foundation library testsuite. A testsuite runner module is available in the [Tool-Project-Template-VEEPort](https://github.com/MicroEJ/Tool-Project-Template-VEEPort/tree/master/vee-port/validation/fs) on Github.

Here is a non exhaustive list of tested environments:

- Hardware
  - STMicroelectronics STM32U5G9 customer board.
- Compilers / Integrated Development Environments:
  - IAR Embedded Workbench 9.70.4
- Passed the Foundation library FS testsuite version 3.0.8


# MISRA Compliance

This Abstraction Layer implementation is MISRA-compliant (MISRA C:2012) with some noted exception.
It has been verified with Cppcheck v2.19. Here is the list of deviations from MISRA standard:

| Deviation  | Category | Justification                                                         |
|:----------:|:--------:|:--------------------------------------------------------------------- |
|  Rule 8.9  | Advisory | Define here for code readability even if it called once in this file. |
|  Rule 11.5 | Advisory | Each LLFS action receives its parameters through the `void *` params field of the async worker job, and converts it back to its own parameter structure. |
|  Rule 11.8 | Required | The OSAL API takes the mutex name as a non-const pointer.             |
|  Rule 17.7 | Required | The use of returned values of debug traces is not necessary           |
|  Rule 18.4 | Advisory | From sni.h with SNI_getArrayLength, used for configurable C library.  |
|  Rule 20.9 | Required | The `__has_include` macro is defined by the toolchain.                |



# Dependencies

- fs-generic 2.1.2 or higher
- File X 6.4.0

# Source

N/A

# Restrictions

None.


---
_Copyright 2025-2026 MicroEJ Corp. All rights reserved._
_Use of this source code is governed by a BSD-style license that can be found with this software._  
_Build: 7E4D1F7C_
