#pragma once

#include <cstddef>
#include <functional>

namespace gfnif {

/*! Runs `body(i)` for every i in [0, count) across `threads` workers.
 *
 *  Deliberately a parallel-for rather than a general task queue: every job in
 *  this pipeline is "convert file i", and indexing by i is what keeps results
 *  independent of completion order (see Orchestrator). Work is handed out one
 *  index at a time from a shared atomic counter, so a file that takes 30x the
 *  average -- some ride/ models carry 87 animation clips -- does not leave a
 *  worker idle at the end of a static partition.
 *
 *  `threads <= 1` runs everything inline on the calling thread, which is what
 *  --debug uses to keep its log ordered.
 *
 *  `body` must be safe to call concurrently. Exceptions escaping `body` would
 *  terminate the process, so it is required not to throw; ConversionJob wraps
 *  its whole body in a catch-all for that reason. */
void ParallelFor(std::size_t count, unsigned threads, const std::function<void(std::size_t)>& body);

/*! hardware_concurrency(), or 1 when the runtime cannot report it. */
unsigned DefaultThreadCount();

} // namespace gfnif
