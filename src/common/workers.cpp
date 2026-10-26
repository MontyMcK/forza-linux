#include "common/workers.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace fl {
namespace {

struct Pool {
    std::mutex run_mutex;  // one parallel_for at a time
    std::mutex m;
    std::condition_variable cv, done_cv;
    const std::function<void(size_t)>* fn = nullptr;
    size_t n = 0;
    std::atomic<size_t> next{0};
    size_t done = 0;
    uint64_t generation = 0;
    std::vector<std::thread> threads;

    void work() {
        for (size_t i; (i = next.fetch_add(1)) < n;) {
            (*fn)(i);
            std::lock_guard lock(m);
            if (++done == n) done_cv.notify_all();
        }
    }

    Pool() {
        unsigned k = std::clamp(std::thread::hardware_concurrency() / 4, 2u, 8u);
        for (unsigned t = 0; t < k; t++)
            threads.emplace_back([this] {
                uint64_t seen = 0;
                for (;;) {
                    {
                        std::unique_lock lock(m);
                        cv.wait(lock, [&] { return generation != seen; });
                        seen = generation;
                    }
                    work();
                }
            });
        for (auto& t : threads) t.detach();
    }
};

Pool& pool() {
    static Pool* p = new Pool();  // never destroyed: workers outlive static destructors
    return *p;
}

}  // namespace

void parallel_for(size_t n, const std::function<void(size_t)>& fn) {
    if (n <= 1) {
        if (n) fn(0);
        return;
    }
    Pool& p = pool();
    std::lock_guard run(p.run_mutex);
    {
        std::lock_guard lock(p.m);
        p.fn = &fn;
        p.n = n;
        p.done = 0;
        p.next = 0;
        p.generation++;
    }
    p.cv.notify_all();
    p.work();
    std::unique_lock lock(p.m);
    p.done_cv.wait(lock, [&] { return p.done == n; });
}

}  // namespace fl
