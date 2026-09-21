#include "pipeline/ThreadPool.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace gfnif {

unsigned DefaultThreadCount() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1u : n;
}

void ParallelFor(std::size_t count, unsigned threads, const std::function<void(std::size_t)>& body) {
    if (count == 0) {
        return;
    }
    if (threads <= 1) {
        for (std::size_t i = 0; i < count; ++i) {
            body(i);
        }
        return;
    }

    const unsigned workerCount =
        static_cast<unsigned>(std::min<std::size_t>(threads, count));

    std::atomic<std::size_t> next{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (unsigned w = 0; w < workerCount; ++w) {
        workers.emplace_back([&next, count, &body] {
            for (;;) {
                const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= count) {
                    return;
                }
                body(i);
            }
        });
    }
    for (std::thread& t : workers) {
        t.join();
    }
}

} // namespace gfnif
