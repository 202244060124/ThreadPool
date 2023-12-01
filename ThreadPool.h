
#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace PoolNs {

class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads = (std::max)(2u, std::thread::hardware_concurrency()));
    template<class F, class... Args>
    auto AddTaskToTaskQueue(F&& f, Args&&... args) -> std::future<
#if defined(__cpp_lib_is_invocable) && __cpp_lib_is_invocable >= 201703
        typename std::invoke_result<F&&, Args&&...>::type
#else
        typename std::result_of<F && (Args && ...)>::type
#endif
        >;
    void WaitTaskQueueEmpty();
    void WaitCurrentTaskFinish();
    void SetQueueSizeLimit(std::size_t size_limit);
    void SetPoolSize(std::size_t size_limit);
    ~ThreadPool();

private:
    void StartWorker(std::size_t worker_number, std::unique_lock<std::mutex> const& lock);

    // need to keep track of threads so we can join them
    std::vector<std::thread> m_workers;
    // target pool size
    std::size_t m_poolSize;
    // the task queue
    std::queue<std::function<void()> > m_tasksQueue;
    // queue length size_limit
    std::size_t m_maxQueueSize = 100000;
    // stop signal
    bool m_stopSignalFlag = false;

    // synchronization
    std::mutex m_queueMutex;
    std::condition_variable m_producersCv;
    std::condition_variable m_consumersCv;

    std::mutex m_currentTaskMutex;
    std::condition_variable m_currentTaskCv;
    std::atomic<std::size_t> m_currentTask;

    struct HandleInRuningTasks {
        ThreadPool& m_threadPool;

        HandleInRuningTasks(ThreadPool& threadPool) : m_threadPool(threadPool)
        {
        }

        ~HandleInRuningTasks()
        {
            std::size_t prev = std::atomic_fetch_sub_explicit(&m_threadPool.m_currentTask, std::size_t(1), std::memory_order_acq_rel);
            if (prev == 1) {
                std::unique_lock<std::mutex> guard(m_threadPool.m_currentTaskMutex);
                m_threadPool.m_currentTaskCv.notify_all();
            }
        }
    };
};

inline ThreadPool::ThreadPool(std::size_t threads) : m_poolSize(threads), m_currentTask(0)
{
    std::unique_lock<std::mutex> lock(this->m_queueMutex);
    for (std::size_t i = 0; i != threads; ++i) {
        StartWorker(i, lock);
    }
}

template<class F, class... Args>
auto ThreadPool::AddTaskToTaskQueue(F&& f, Args&&... args) -> std::future<
#if defined(__cpp_lib_is_invocable) && __cpp_lib_is_invocable >= 201703
    typename std::invoke_result<F&&, Args&&...>::type
#else
    typename std::result_of<F && (Args && ...)>::type
#endif
    >
{
#if defined(__cpp_lib_is_invocable) && __cpp_lib_is_invocable >= 201703
    using return_type = typename std::invoke_result<F&&, Args&&...>::type;
#else
    using return_type = typename std::result_of<F && (Args && ...)>::type;
#endif

    auto task = std::make_shared<std::packaged_task<return_type()> >(std::bind(std::forward<F>(f), std::forward<Args>(args)...));

    std::future<return_type> res = task->get_future();

    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (m_tasksQueue.size() >= m_maxQueueSize) {
        // wait for the queue to empty or be stopped
        m_producersCv.wait(lock, [this] { return m_tasksQueue.size() < m_maxQueueSize || m_stopSignalFlag; });
    }

    // don't allow enqueueing after stopping the pool
    if (m_stopSignalFlag) {
        throw std::runtime_error("AddTaskToTaskQueue on stopped ThreadPool");
    }

    m_tasksQueue.emplace([task]() { (*task)(); });
    std::atomic_fetch_add_explicit(&m_currentTask, std::size_t(1), std::memory_order_relaxed);
    m_consumersCv.notify_one();

    return res;
}

inline ThreadPool::~ThreadPool()
{
    std::unique_lock<std::mutex> lock(m_queueMutex);
    m_stopSignalFlag = true;
    m_poolSize = 0;
    m_consumersCv.notify_all();
    m_producersCv.notify_all();
    m_consumersCv.wait(lock, [this] { return this->m_workers.empty(); });
    assert(m_currentTask == 0);
}

inline void ThreadPool::WaitTaskQueueEmpty()
{
    std::unique_lock<std::mutex> lock(this->m_queueMutex);
    this->m_producersCv.wait(lock, [this] { return this->m_tasksQueue.empty(); });
}

inline void ThreadPool::WaitCurrentTaskFinish()
{
    std::unique_lock<std::mutex> lock(this->m_currentTaskMutex);
    this->m_currentTaskCv.wait(lock, [this] { return this->m_currentTask == 0; });
}

inline void ThreadPool::SetQueueSizeLimit(std::size_t size_limit)
{
    std::unique_lock<std::mutex> lock(this->m_queueMutex);

    if (m_stopSignalFlag) {
        return;
    }

    std::size_t const old_limit = m_maxQueueSize;
    m_maxQueueSize = (std::max)(size_limit, std::size_t(1));
    if (old_limit < m_maxQueueSize) {
        m_producersCv.notify_all();
    }
}

inline void ThreadPool::SetPoolSize(std::size_t size_limit)
{
    if (size_limit < 1) {
        size_limit = 1;
    }

    std::unique_lock<std::mutex> lock(this->m_queueMutex);

    if (m_stopSignalFlag) {
        return;
    }

    std::size_t const old_size = m_poolSize;
    assert(this->m_workers.size() >= old_size);

    m_poolSize = size_limit;
    if (m_poolSize > old_size) {
        // create new worker threads
        // it is possible that some of these are still running because
        // they have not stopped yet after a pool size reduction, such
        // m_workers will just keep running
        for (std::size_t i = old_size; i != m_poolSize; ++i) {
            StartWorker(i, lock);
        }
    } else if (m_poolSize < old_size) {
        // notify all worker threads to start downsizing
        this->m_consumersCv.notify_all();
    }
}

inline void ThreadPool::StartWorker(std::size_t worker_number, std::unique_lock<std::mutex> const& lock)
{
    assert(lock.owns_lock() && lock.mutex() == &this->m_queueMutex);
    assert(worker_number <= this->m_workers.size());

    auto worker_func = [this, worker_number] {
        for (;;) {
            std::function<void()> task;
            bool notify;
            {
                std::unique_lock<std::mutex> lock(this->m_queueMutex);
                this->m_consumersCv.wait(lock, [this, worker_number] {
                    return this->m_stopSignalFlag || !this->m_tasksQueue.empty() || m_poolSize < worker_number + 1;
                });

                // deal with downsizing of thread pool or shutdown
                if ((this->m_stopSignalFlag && this->m_tasksQueue.empty()) || (!this->m_stopSignalFlag && m_poolSize < worker_number + 1)) {
                    // detach this worker, effectively marking it stopped
                    this->m_workers[worker_number].detach();
                    // downsize the m_workers vector as much as possible
                    while (this->m_workers.size() > m_poolSize && !this->m_workers.back().joinable())
                        this->m_workers.pop_back();
                    // if this is was last worker, notify the destructor
                    if (this->m_workers.empty())
                        this->m_consumersCv.notify_all();
                    return;
                } else if (!this->m_tasksQueue.empty()) {
                    task = std::move(this->m_tasksQueue.front());
                    this->m_tasksQueue.pop();
                    notify = this->m_tasksQueue.size() + 1 == m_maxQueueSize || this->m_tasksQueue.empty();
                } else {
                    continue;
                }
            }

            HandleInRuningTasks guard(*this);

            if (notify) {
                std::unique_lock<std::mutex> lock(this->m_queueMutex);
                m_producersCv.notify_all();
            }

            task();
        }
    };

    if (worker_number < this->m_workers.size()) {
        std::thread& worker = this->m_workers[worker_number];
        // start only if not already running
        if (!worker.joinable()) {
            worker = std::thread(worker_func);
        }
    } else {
        this->m_workers.push_back(std::thread(worker_func));
    }
}

} // namespace PoolNs

#endif // THREAD_POOL_H