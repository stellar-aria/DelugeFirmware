// tests/spec_io/mock_file_io.h
#pragma once

extern "C" {
#include "libdeluge/file_io.h"
}

/// Test-only: clears all mock state. Call at the start of every spec that
/// touches the mock, so cases don't see each other's files/directories.
void mock_file_io_reset();

/// Test-only: force `path` to fail with `status` on open/dir_open/mkdir/rename, even when the
/// path EXISTS in the mock. Models the Rust BSP's off-fiber `DELUGE_ERR_BUSY` rejection, where the
/// filesystem refuses to answer rather than reporting absence.
void mock_file_io_inject_status(const char* path, DelugeStatus status);

/// Test-only: drop all injections (also done by mock_file_io_reset()).
void mock_file_io_clear_injections();
