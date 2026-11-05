# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2026-11-05

### Added

- Add support for opening a file in read-write mode.
- Allow the VEE Port configuration to override the maximum number of open files (`FS_MAX_NUMBER_OF_FILE_IN_POOL`).
- Add support for FileX in standalone mode (`FX_STANDALONE_ENABLE`), for an RTOS other than ThreadX.

### Changed

- Update the FS Generic C module dependency to version 2.3.0.
- Update the Async Worker dependency to version 0.6.0.
- Report file system errors with the standard exception codes instead of an error message.

### Fixed

- Fix two files that get the same FileX file object when two threads open files at the same time.
- Fix a crash, or a read or write on another file, when a stream is used after it is closed.
- Fix a file left open in FileX when the opening of a file fails.
- Fix a seek beyond 4 GiB that moves the file pointer inside the file instead of at its end.

## [1.0.0] - 2025-04-15

- Initial release of the CCO module based on CCO Generic template 0.1.0.

---
_Copyright 2023-2026 MicroEJ Corp. All rights reserved._
_Use of this source code is governed by a BSD-style license that can be found with this software._
_Build: 7E4D1F7C_
