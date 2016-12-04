#include <atomic>
#include <iostream>
#include <thread>

// Set either of these to 1 to prevent CPU reordering
#define USE_CPU_FENCE              0
#define USE_SINGLE_HW_THREAD       0  // Supported on Linux, but not Cygwin or PS3

#if USE_SINGLE_HW_THREAD
#include <pthread.h>
#include <sched.h>
#endif


//-------------------------------------
//  MersenneTwister
//  A thread-safe random number generator with good randomness
//  in a small number of instructions. We'll use it to introduce
//  random timing delays.
//-------------------------------------
#define MT_IA  397
#define MT_LEN 624

class MersenneTwister
{
    unsigned int m_buffer[MT_LEN];
    int m_index;

public:
    MersenneTwister(unsigned int seed);
    // Declare noinline so that the function call acts as a compiler barrier:
    unsigned int integer() __attribute__((noinline));
};

MersenneTwister::MersenneTwister(unsigned int seed)
{
    // Initialize by filling with the seed, then iterating
    // the algorithm a bunch of times to shuffle things up.
    for (int i = 0; i < MT_LEN; i++)
        m_buffer[i] = seed;
    m_index = 0;
    for (int i = 0; i < MT_LEN * 100; i++)
        integer();
}

unsigned int MersenneTwister::integer()
{
    // Indices
    int i = m_index;
    int i2 = m_index + 1; if (i2 >= MT_LEN) i2 = 0; // wrap-around
    int j = m_index + MT_IA; if (j >= MT_LEN) j -= MT_LEN; // wrap-around

    // Twist
    unsigned int s = (m_buffer[i] & 0x80000000) | (m_buffer[i2] & 0x7fffffff);
    unsigned int r = m_buffer[j] ^ (s >> 1) ^ ((s & 1) * 0x9908B0DF);
    m_buffer[m_index] = r;
    m_index = i2;

    // Swizzle
    r ^= (r >> 11);
    r ^= (r << 7) & 0x9d2c5680UL;
    r ^= (r << 15) & 0xefc60000UL;
    r ^= (r >> 18);
    return r;
}


//-------------------------------------
//  Main program, as decribed in the post
//-------------------------------------
std::atomic<int> beginSema1;
std::atomic<int> beginSema2;
std::atomic<int> endSema;

int X, Y;
int r1, r2;

void *sem_signal(std::atomic<int>& sema) {
    sema.fetch_add(1, std::memory_order_release);
    return nullptr;
}

void *sem_wait(std::atomic<int>& sema) {
    int oldCount;

    while(true) {
        oldCount = sema.load(std::memory_order_relaxed);
        if (oldCount > 0 && sema.compare_exchange_strong(oldCount, oldCount - 1, std::memory_order_acquire)) {
            break;
        }
        sched_yield();
    }
    return nullptr;
}

void *thread1Func()
{
    MersenneTwister random(1);
    for (;;)
    {
        sem_wait(beginSema1);
        while (random.integer() % 8 != 0) {}  // Random delay

        // ----- THE TRANSACTION! -----
        X = 1;
#if USE_CPU_FENCE
        asm volatile("mfence" ::: "memory");  // Prevent CPU reordering
#else
        asm volatile("" ::: "memory");  // Prevent compiler reordering
#endif
        r1 = Y;

        sem_signal(endSema);  // Notify transaction complete
    }
    return NULL;  // Never returns
};

void *thread2Func()
{
    MersenneTwister random(2);
    for (;;)
    {
        sem_wait(beginSema2);
        while (random.integer() % 8 != 0) {}  // Random delay

        // ----- THE TRANSACTION! -----
        Y = 1;
#if USE_CPU_FENCE
        asm volatile("mfence" ::: "memory");  // Prevent CPU reordering
#else
        asm volatile("" ::: "memory");  // Prevent compiler reordering
#endif
        r2 = X;

        sem_signal(endSema);  // Notify transaction complete
    }
    return NULL;  // Never returns
};

int main()
{
    // Initialize the semaphores
    beginSema1 = 0;
    beginSema2 = 0;
    endSema = 0;

    // Spawn the threads
    std::thread thread1(thread1Func);
    std::thread thread2(thread2Func);

#if USE_SINGLE_HW_THREAD
    // Force thread affinities to the same cpu core.
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus);
    pthread_setaffinity_np(thread1.native_handle(), sizeof(cpu_set_t), &cpus);
    pthread_setaffinity_np(thread2.native_handle(), sizeof(cpu_set_t), &cpus);
#endif

    // Repeat the experiment ad infinitum
    int detected = 0;
    for (int iterations = 1; ; iterations++)
    {
        // Reset X and Y
        X = 0;
        Y = 0;
        // Signal both threads
        sem_signal(beginSema1);
        sem_signal(beginSema2);

        // Wait for both threads
        sem_wait(endSema);
        sem_wait(endSema);
        // Check if there was a simultaneous reorder
        if (r1 == 0 && r2 == 0)
        {
            detected++;
            std::cout << detected << " reorders detected after " << iterations << " iterations\n";
            std::cout << std::endl;
        }
    }
    return 0;  // Never returns
}
