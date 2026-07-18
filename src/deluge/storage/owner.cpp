/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "storage/owner.h"

#include "libdeluge/worker.h"

namespace deluge::storage {

void Owner::run(void (*fn)(void*), void* ctx) {
	deluge_worker_run(fn, ctx);
}

void Coalescer::request(void (*fill)(void*), void* ctx) {
	if (in_flight_.exchange(true, std::memory_order_acq_rel)) {
		return; // a dispatch is already in flight — it covers this demand
	}
	fill_ = fill;
	ctx_ = ctx;
	Owner::run(&Coalescer::run_and_release, this);
}

void Coalescer::run_and_release(void* self_) {
	auto* self = static_cast<Coalescer*>(self_);
	self->fill_(self->ctx_);
	self->in_flight_.store(false, std::memory_order_release);
}

} // namespace deluge::storage
