#pragma once
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>
#include <vector>

namespace engine::jobs {

// Simple thread pool for CPU-bound background work (terrain mesh generation,
// procedural placement, etc.). Not for GPU/OpenGL calls — those must stay on the main thread.
class ThreadPool {
public:
    explicit ThreadPool(size_t workerCount);
    ~ThreadPool();

    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Enqueue a job; runs on any worker thread.
    void Submit(std::function<void()> job);

    // Number of jobs queued or currently running.
    size_t PendingJobs() const { return pending.load(std::memory_order_relaxed); }

    size_t WorkerCount() const { return workers.size(); }

private:
    std::vector<std::thread>          workers;
    std::queue<std::function<void()>> queue;
    mutable std::mutex                mtx;
    std::condition_variable           cv;
    std::atomic<bool>                 stop{false};
    std::atomic<size_t>               pending{0};

    void workerLoop();
};

// Lazily-constructed global pool sized to hardware_concurrency() - 1 (min 1),
// leaving one core for the main render thread.
ThreadPool& GetGlobalPool();

}  // namespace engine::jobs
