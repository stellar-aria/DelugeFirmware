#pragma once
#include "definitions.h"
#include "util/container/list/bidirectional_linked_list.h"


namespace deluge::memory {

class StealableManager {
public:
	BidirectionalLinkedList stealableClusterQueues[NUM_STEALABLE_QUEUES];
	// Keeps track, semi-accurately, of biggest runs of memory that could be stolen. In a perfect world, we'd have a second
	// index on stealableClusterQueues[q], for run length. Although even that wouldn't automatically reflect changes to run
	// lengths as neighbouring memory is allocated.
	uint32_t stealableClusterQueueLongestRuns[NUM_STEALABLE_QUEUES];
};

}
