// tests/spec_io/mock_file_io.h
#pragma once

/// Test-only: clears all mock state. Call at the start of every spec that
/// touches the mock, so cases don't see each other's files/directories.
void mock_file_io_reset();
