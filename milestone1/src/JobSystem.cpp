#include "engine/JobSystem.hpp"

namespace engine {

JobSystem::JobSystem(int workerCount) {
    if (workerCount < 1) {
        workerCount = 1;
    }
    workers.reserve(static_cast<size_t>(workerCount));
    for (int i = 0; i < workerCount; ++i) {
        workers.emplace_back(&JobSystem::WorkerLoop, this);
    }
}

JobSystem::~JobSystem() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void JobSystem::Submit(Job job) {
    jobs.push_back(std::move(job));
}

// The ThreadExample's worker(), unchanged in shape: pull an index, run that
// job, repeat until the index runs off the end. `count` is this frame's job
// count, snapshotted by the caller under the mutex at the same moment it saw
// the generation - so a worker can never compare a fresh index against a
// stale count.
void JobSystem::Drain(size_t count) {
    while (true) {
        const size_t index = nextJobIndex.fetch_add(1);
        if (index >= count) {
            break;
        }
        jobs[index]();
        jobsFinished.fetch_add(1);
    }
}

void JobSystem::WorkerLoop() {
    uint64_t seen = 0;
    while (true) {
        size_t count = 0;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return stopping || generation != seen; });
            if (stopping) {
                return;
            }
            // Seeing the generation, reading its count, and being counted as
            // awake all happen under one lock - that is what lets RunAndWait
            // know no worker can still wake up late for a finished frame.
            seen = generation;
            count = jobCount;
            ++acknowledged;
            ++draining;
        }

        Drain(count);

        {
            std::lock_guard<std::mutex> lock(mutex);
            --draining;
        }
        done.notify_all();
    }
}

void JobSystem::RunAndWait() {
    const size_t count = jobs.size();
    if (count == 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        jobsFinished.store(0);
        nextJobIndex.store(0);
        jobCount = count;
        acknowledged = 0;
        ++generation;
    }
    wake.notify_all();

    // The producer is a worker too. With few jobs it will often finish the
    // whole list before a parked worker has even been scheduled.
    Drain(count);

    // The barrier. Every job has run, every worker has acknowledged THIS
    // generation, and none is still inside Drain. Only then is it safe to
    // clear the vector and, next frame, reset the atomics - a worker that has
    // acknowledged N cannot wake for N again, and one that never woke for N
    // will wake straight into N+1 with N+1's count.
    {
        std::unique_lock<std::mutex> lock(mutex);
        done.wait(lock, [&] {
            return jobsFinished.load() >= count &&
                   acknowledged == static_cast<int>(workers.size()) &&
                   draining == 0;
        });
    }

    jobs.clear();
}

} // namespace engine
