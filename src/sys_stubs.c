#include "RTT/SEGGER_RTT.h"
#include "definitions.h"
#include <sys/stat.h>

// this is not included in the build but remains as a repo of information for implementing these in the future,
// and to help troubleshoot link failures in the future - e.g. if _sbrk is required, it can be added to
// finish compilation and find out what's including it

// this stub fails to allocate - needed for libc malloc
// Take advantage of that to ensure anything which allocates will fail to link
void* _sbrk(int incr) {
	return (void*)-1;
}

// needed for libc abort, raise, return from main
void _exit(int status) {
	// halt execution
	__builtin_unreachable();
}

// needed for libc abort
void _kill(int pid, int sig) {
	return;
}

// needed for libc abort
int _getpid(void) {
	return -1;
}

// needed for stdio, files
int _close(int file) {
	return -1;
}

// return character oriented (not block)
int _fstat(int file, struct stat* st) {
	st->st_mode = S_IFCHR;
	return 0;
}

// return character oriented
int _isatty(int file) {
	return 1;
}

int _lseek(int file, int ptr, int dir) {
	return 0;
}
// Redirect to RTT for TinyUSB debug output
int _write(int file, char* ptr, int len) {
	(void)file;
	if (len > 0) {
		SEGGER_RTT_Write(0, ptr, len);
	}
	return len;
}
// read nothing
int _read(int file, char* ptr, int len) {
	return 0;
}
