#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
#include "ThreadPool.h"
using namespace std;
using namespace PoolNs;

mutex g_mutex;
queue<int> g_queue;
condition_variable g_cv;

int Producer(void)
{
    int i = 0;
    while (1) {
        unique_lock<mutex> lock(g_mutex);
        while (g_queue.size() == 10000) {
            g_cv.wait(lock);
        }

        cout << "Producer: " << i << endl;
        g_queue.emplace(i);
        lock.unlock();
        i++;
        if (i == 100000) {
            i = 0;
        }
        g_cv.notify_all();
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    return 0;
}

int Consumer(void)
{
    int out = 0;
    while (1) {
        unique_lock<mutex> lock(g_mutex);
        while (g_queue.empty()) {
            g_cv.wait(lock);
        }
        out = g_queue.front();
        g_queue.pop();
        lock.unlock();
        cout << "Consumer: " << out << endl;
        g_cv.notify_all();
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    return 0;
}

int main(void)
{
    ThreadPool pool;
    std::vector<std::future<int> > results;

    for (int i = 0; i < 30; ++i) {
        results.emplace_back(pool.AddTaskToTaskQueue(Producer));
        results.emplace_back(pool.AddTaskToTaskQueue(Consumer));
    }

    pool.WaitTaskQueueEmpty();
    pool.WaitCurrentTaskFinish();

    for (auto&& result : results) {
        std::cout << result.get() << ' ';
    }
    std::cout << std::endl;

    return 0;
}
