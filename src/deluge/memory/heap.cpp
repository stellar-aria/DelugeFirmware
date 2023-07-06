#include "heap.h"
#include "definitions.h"

extern const size_t __heap_start__;

namespace deluge::memory {

Heap internal{reinterpret_cast<size_t>(&__heap_start__), INTERNAL_MEMORY_END - PROGRAM_STACK_MAX_SIZE};
Heap SDRAM{EXTERNAL_MEMORY_BEGIN, EXTERNAL_MEMORY_END};

}
