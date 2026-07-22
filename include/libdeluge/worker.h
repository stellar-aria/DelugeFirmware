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

/// libdeluge/worker.h — run a long, cooperatively-yielding operation off the
/// caller's stack.
///
/// Some user-initiated operations (song load, stem export, grid clip create) run
/// for a long time and `yield()` (scheduler_api.h) partway through to let other
/// work proceed. On a runtime where the caller's context cannot itself suspend
/// (e.g. the Embassy BSP, where the operation is dispatched from a task that must
/// stay responsive), hand the operation to the worker: it runs on a dedicated
/// stackful fiber so its `yield()`s suspend the *operation*, not the caller.
///
/// `deluge_worker_run` returns immediately (the operation is queued); the launching
/// handler must therefore include any post-operation work *inside* `fn`.
#ifndef LIBDELUGE_WORKER_H
#define LIBDELUGE_WORKER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Queue `fn(ctx)` to run on the cooperative worker. Operations are serialized.
/// `fn` may `yield()` at any call depth; the launching caller returns at once.
/// @return true if the operation ran (cooperative/inline) or was queued (Embassy
///         worker); false if it was dropped because the worker queue was full and
///         will NOT run — a coalescing caller must treat false as "not dispatched".
bool deluge_worker_run(void (*fn)(void*), void* ctx);

/// Like deluge_worker_run, but the operation is an SD-routine-class op: for its
/// whole in-flight window (enqueue → completion) the worker holds off
/// RESOURCE_SD_ROUTINE scheduler tasks, so a task that frees an object the op is
/// mid-way through (e.g. the recorder, freed by discardRecorder) cannot run
/// concurrently with it. Cooperative/host: identical to deluge_worker_run (inline).
/// Embassy: increments an SD-routine hold at enqueue, released at completion.
/// @return as deluge_worker_run: true if it ran/queued, false if dropped (queue
///         full) and will NOT run — on false NOTHING was enqueued and no hold was taken.
bool deluge_worker_run_sd_routine(void (*fn)(void*), void* ctx);

/// Like deluge_worker_run, but the operation is HIGH-priority: it dequeues ahead of
/// every already-queued or later-queued deluge_worker_run/deluge_worker_run_sd_routine
/// (NORMAL) op, FIFO among other HIGH ops. For audio-streaming reads, which must not
/// queue behind UI/recorder work on the shared worker ring. Does NOT take an
/// SD-routine hold — priority and the SD-routine hold are orthogonal.
/// Cooperative/host: identical to deluge_worker_run (inline) — legacy/host has no
/// queue, so priority is a no-op there.
/// @return as deluge_worker_run: true if it ran/queued, false if dropped (queue full)
///         and will NOT run — on false NOTHING was enqueued.
bool deluge_worker_run_priority(void (*fn)(void*), void* ctx);

/// Is a HIGH-priority op currently queued (waiting to run) on the worker? A
/// long-running caller that processes several independent units per dispatch
/// (e.g. audio_engine::doRecorderCardRoutines, which walks every live
/// SampleRecorder and drives one cardRoutine() each — each recorder's own
/// per-dispatch write is already bounded to a single cluster) can check this
/// after fully processing one unit and, if true, stop for this dispatch
/// rather than continuing to the next unit. The next dispatch resumes the
/// traversal where it left off, so a queued HIGH op (an audio-streaming
/// read) is bounded to waiting behind at most one more unit instead of the
/// whole multi-unit loop.
/// Cooperative/host: always returns false — there is no queue, so there is
/// never a reason to step aside (inline behaviour is unchanged: the caller
/// drains fully, exactly as before this existed).
/// Embassy: true while any deluge_worker_run_priority-submitted op is resident
/// in the worker ring (queued, not yet dequeued).
bool deluge_worker_higher_priority_waiting(void);

/// True iff the calling context IS the storage worker (the worker fiber on the
/// Embassy BSP). Lets a dual-context caller skip re-dispatching when it is
/// already on the worker — dispatching again would nest a queued op inside the
/// running one (Embassy) and reorder or deadlock. Cooperative/host: always
/// false, because `deluge_worker_run` runs inline there, so nesting is harmless
/// and no caller needs to branch on this.
bool deluge_worker_on_worker(void);

#ifdef __cplusplus
}
#endif

#endif // LIBDELUGE_WORKER_H
