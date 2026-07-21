#include "engine/jobs/thread_pool.hpp"

namespace engine::jobs {

ThreadPool::ThreadPool(size_t workerCount) {
    if (workerCount < 1) workerCount = 1;
    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i) {
        workers.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mtx);
        stop.store(true, std::memory_order_release);
    }
    cv.notify_all();
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}

void ThreadPool::Submit(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        queue.push(std::move(job));
        pending.fetch_add(1, std::memory_order_relaxed);
    }
    cv.notify_one();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] {
                return stop.load(std::memory_order_acquire) || !queue.empty();
            });
            if (stop.load(std::memory_order_acquire) && queue.empty()) return;
            job = std::move(queue.front());
            queue.pop();
        }
        job();
        pending.fetch_sub(1, std::memory_order_relaxed);
    }
}

ThreadPool& GetGlobalPool() {
    static ThreadPool pool([] {
        unsigned n = std::thread::hardware_concurrency();
        if (n <= 1) return size_t{1};
        return size_t{n - 1};
    }());
    return pool;
}

}  // namespace engine::jobs
