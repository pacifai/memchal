#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace mc {

// Threads that live across calls to run(). The calling thread works as worker 0.
class ThreadPool {
public:
    explicit ThreadPool(uint32_t workers) {
        for (uint32_t w = 1; w < workers; ++w) threads_.emplace_back([this, w] { loop(w); });
    }
    ~ThreadPool() {
        { std::lock_guard<std::mutex> g(m_); stop_ = true; }
        start_.notify_all();
        for (auto& t : threads_) t.join();
    }
    uint32_t size() const { return (uint32_t)threads_.size() + 1; }

    // Calls job(worker, item) for every item in [0, items) and returns when all are done.
    void run(uint32_t items, const std::function<void(uint32_t, uint32_t)>& job) {
        if (items == 0) return;
        if (threads_.empty() || items == 1) {
            for (uint32_t i = 0; i < items; ++i) job(0, i);
            return;
        }
        {
            std::lock_guard<std::mutex> g(m_);
            job_ = &job; items_ = items; next_ = 0;
            busy_ = (uint32_t)threads_.size();
            ++generation_;
        }
        start_.notify_all();
        work(0);
        std::unique_lock<std::mutex> g(m_);
        done_.wait(g, [this] { return busy_ == 0; });
        job_ = nullptr;
    }

private:
    void work(uint32_t w) {
        for (uint32_t i; (i = next_.fetch_add(1)) < items_;) (*job_)(w, i);
    }
    void loop(uint32_t w) {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> g(m_);
                start_.wait(g, [&] { return stop_ || generation_ != seen; });
                if (stop_) return;
                seen = generation_;
            }
            work(w);
            std::lock_guard<std::mutex> g(m_);
            if (--busy_ == 0) done_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable start_, done_;
    const std::function<void(uint32_t, uint32_t)>* job_ = nullptr;
    std::atomic<uint32_t> next_{0};
    uint32_t items_ = 0, busy_ = 0;
    uint64_t generation_ = 0;
    bool stop_ = false;
};

} // namespace mc
