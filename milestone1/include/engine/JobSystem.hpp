#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace engine {

// Section 3: a persistent pool of worker threads that drain a per-frame list
// of jobs.
//
// Attribution: the drain loop is adapted from the CSC 481/581 Module 6
// ThreadExample (AlexanderCard/CSC481-581-M6, src/JobSystem.cpp). Its
// worker() hands out work with nextJobIndex.fetch_add(1): because the
// increment is atomic, two workers can never be given the same index, so no
// mutex is needed to distribute jobs. That loop is kept as-is in Drain().
//
// What is built on top is persistence. The example spawns workers, drains one
// list and joins them - fine for a batch, but a game loop does this a hundred
// times a second and a thread spawn costs tens of microseconds. So the workers
// here are created once and parked on a condition variable between frames.
// RunAndWait() wakes them, drains alongside them so the calling thread is not
// idle, and returns once every job has finished.
//
// Persistence has a cost the one-shot version does not: a worker can wake up
// late for a frame that has already finished. If it then pulled a fresh index
// against a stale count it would skip a job (hanging the producer) or index
// past the end of the vector. So RunAndWait ends with a barrier - every worker
// must acknowledge the current generation and leave Drain() before the frame
// is considered over. That is a lightweight stand-in for the example's join().
//
// Contract: Submit() and RunAndWait() are called from one thread - the
// producer - and never concurrently with each other. Jobs may run on any
// worker, so anything two jobs share must be safe to share; the intended use
// is jobs over disjoint slices of the world.
class JobSystem {
public:
    using Job = std::function<void()>;

    explicit JobSystem(int workerCount);
    ~JobSystem();

    JobSystem(const JobSystem &) = delete;
    JobSystem &operator=(const JobSystem &) = delete;

    void Submit(Job job);

    // Runs every submitted job across the pool and the calling thread, blocks
    // until all have finished, then empties the list for the next frame.
    void RunAndWait();

    int WorkerCount() const { return static_cast<int>(workers.size()); }

private:
    void WorkerLoop();

    // The ThreadExample's worker(): pull an index, run that job, repeat until
    // the index runs off the end of this frame's list.
    void Drain(size_t count);

    std::vector<Job> jobs;

    // The ThreadExample's SharedData::nextJobIndex, and the count of jobs run.
    std::atomic<size_t> nextJobIndex{0};
    std::atomic<size_t> jobsFinished{0};

    // Everything below is guarded by `mutex`.
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable done;
    size_t jobCount = 0;      // this frame's job count; workers snapshot it on waking
    uint64_t generation = 0;  // bumped once per RunAndWait
    int acknowledged = 0;     // workers that have woken for this generation
    int draining = 0;         // workers currently inside Drain()
    bool stopping = false;

    std::vector<std::thread> workers;
};

} // namespace engine
