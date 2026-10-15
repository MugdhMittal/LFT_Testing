/**
 * Author: Trevor Brown (me [at] tbrown [dot] pro).
 * Copyright 2018.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <iostream>
#include "random_xoshiro256p.h"
#include "adapter.h"
#include <omp.h>

thread_local Random64 rng;
thread_local int tid;

int main(int argc, char** argv) {
    const int           MAX_KEY = 1000000;
    const int           NUM_THREADS = omp_get_max_threads();

    const int           KEY_ANY = 0; // should be an unused key (that you won't insert)
    const int           unused1 = 0;
    void *              unused2 = NULL;
    Random64 * const    unused3 = NULL;
    auto                tree = new ds_adapter<int, void *>(NUM_THREADS, KEY_ANY, unused1, unused2, unused3);

    #pragma omp parallel
    {
        tid = omp_get_thread_num();
        tree->initThread(tid);
        rng.setSeed(tid+1);
    }

    #pragma omp parallel for
    for (int i=0;i<MAX_KEY;++i) {
        int key = 1+rng.next(MAX_KEY);
        tree->insertIfAbsent(tid, key, (void*)(size_t)key);
    }

    #pragma omp parallel for
    for (int i=0;i<1000000;++i) {
        int key = 1+rng.next(MAX_KEY);
        if (rng.next(2))    tree->insertIfAbsent(tid, key, (void*)(size_t)key);
        else                tree->erase(tid, key);
    }

    #pragma omp parallel for
    for (int key=1;key<=MAX_KEY;++key) {
        tree->erase(tid, key);
    }

    #pragma omp parallel
    {
        tree->deinitThread(tid);
    }

    delete tree;

    std::cout<<"Passed prefill, mix, deleteAll tests."<<std::endl;
    return 0;
}
