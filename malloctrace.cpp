/*
 * Copyright 2014 Milian Wolff <mail@milianw.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <cstdio>
#include <cassert>
#include <cstring>
#include <cstdlib>

#include <unordered_map>
#include <atomic>
#include <string>

#include <dlfcn.h>
#include <unistd.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

using namespace std;

namespace {

using malloc_t = void* (*) (size_t);
using free_t = void (*) (void*);
using realloc_t = void* (*) (void*, size_t);
using calloc_t = void* (*) (size_t, size_t);
using posix_memalign_t = int (*) (void **, size_t, size_t);
using valloc_t = void* (*) (size_t);
using aligned_alloc_t = void* (*) (size_t, size_t);

malloc_t real_malloc = nullptr;
free_t real_free = nullptr;
realloc_t real_realloc = nullptr;
calloc_t real_calloc = nullptr;
posix_memalign_t real_posix_memalign = nullptr;
valloc_t real_valloc = nullptr;
aligned_alloc_t real_aligned_alloc = nullptr;

struct IPCacheEntry
{
    size_t id;
    bool skip;
    bool stop;
};

atomic<size_t> next_cache_id;
atomic<size_t> next_thread_id;

// must be kept separately from ThreadData to ensure it stays valid
// even until after ThreadData is destroyed
thread_local bool in_handler = false;

string env(const char* variable)
{
    const char* value = getenv(variable);
    return value ? string(value) : string();
}

//TODO: per-thread output
struct ThreadData
{
    ThreadData()
        : thread_id(next_thread_id++)
        , out(nullptr)
    {
        bool wasInHandler = in_handler;
        in_handler = true;
        ipCache.reserve(1024);
        string outputFileName = env("DUMP_MALLOC_TRACE_OUTPUT") + to_string(getpid()) + '.' + to_string(thread_id);
        out = fopen(outputFileName.c_str(), "wa");
        if (!out) {
            fprintf(stderr, "Failed to open output file: %s\n", outputFileName.c_str());
            exit(1);
        }
        in_handler = wasInHandler;
    }

    ~ThreadData()
    {
        in_handler = true;
        fclose(out);
    }

    unordered_map<unw_word_t, IPCacheEntry> ipCache;
    size_t thread_id;
    FILE* out;
};

thread_local ThreadData threadData;

void print_caller()
{
    unw_context_t uc;
    unw_getcontext (&uc);

    unw_cursor_t cursor;
    unw_init_local (&cursor, &uc);

    // skip handleMalloc & malloc
    for (int i = 0; i < 2; ++i) {
        if (unw_step(&cursor) <= 0) {
            return;
        }
    }

    auto& ipCache = threadData.ipCache;

    while (unw_step(&cursor) > 0)
    {
        unw_word_t ip;
        unw_get_reg(&cursor, UNW_REG_IP, &ip);

        // check cache for known ip's
        auto it = ipCache.find(ip);
        if (it == ipCache.end()) {
            // not cached yet, get data
            const size_t BUF_SIZE = 256;
            char name[BUF_SIZE];
            name[0] = '\0';
            unw_word_t offset;
            unw_get_proc_name(&cursor, name, BUF_SIZE, &offset);
            // skip operator new (_Znwm) and operator new[] (_Znam)
            const bool skip = name[0] == '_' && name[1] == 'Z' && name[2] == 'n'
                        && name[4] == 'm' && name[5] == '\0'
                        && (name[3] == 'w' || name[3] == 'a');
            const bool stop = !skip && (!strcmp(name, "main") || !strcmp(name, "_GLOBAL__sub_I_main"));
            const size_t id = next_cache_id++;

            // and store it in the cache
            ipCache.insert(it, make_pair(ip, IPCacheEntry{id, skip, stop}));

            if (!skip) {
                fprintf(threadData.out, "%lu=%lx@%s+0x%lx;", id, ip, name, offset);
            }
            if (stop) {
                break;
            }
            continue;
        }

        const auto& frame = it->second;
        if (!frame.skip) {
            fprintf(threadData.out, "%lu;", frame.id);
        }
        if (frame.stop) {
            break;
        }
    }
}

template<typename T>
T findReal(const char* name)
{
    auto ret = dlsym(RTLD_NEXT, name);
    if (!ret) {
        fprintf(stderr, "could not find original function %s\n", name);
        exit(1);
    }
    return reinterpret_cast<T>(ret);
}

/**
 * Dummy implementation, since the call to dlsym from findReal triggers a call to calloc.
 *
 * This is only called at startup and will eventually be replaced by the "proper" calloc implementation.
 */
void* dummy_calloc(size_t num, size_t size)
{
    const size_t MAX_SIZE = 1024;
    static char* buf[MAX_SIZE];
    static size_t offset = 0;
    if (!offset) {
        memset(buf, 0, MAX_SIZE);
    }
    size_t oldOffset = offset;
    offset += num * size;
    if (offset >= MAX_SIZE) {
        fprintf(stderr, "failed to initialize, dummy calloc buf size exhausted: %lu requested, %lu available\n", offset, MAX_SIZE);
        exit(1);
    }
    return buf + oldOffset;
}

void init()
{
    if (in_handler) {
        fprintf(stderr, "initialization recursion detected\n");
        exit(1);
    }

    in_handler = true;

    real_calloc = &dummy_calloc;
    real_calloc = findReal<calloc_t>("calloc");
    real_malloc = findReal<malloc_t>("malloc");
    real_free = findReal<free_t>("free");
    real_realloc = findReal<realloc_t>("realloc");
    real_posix_memalign = findReal<posix_memalign_t>("posix_memalign");
    real_valloc = findReal<valloc_t>("valloc");
    real_aligned_alloc = findReal<aligned_alloc_t>("aligned_alloc");

    in_handler = false;
}

void handleMalloc(void* ptr, size_t size)
{
    fprintf(threadData.out, "+%ld:%p ", size, ptr);
    print_caller();
    fprintf(threadData.out,"\n");
}

void handleFree(void* ptr)
{
    fprintf(threadData.out, "-%p\n", ptr);
}

}

extern "C" {

/// TODO: memalign, pvalloc, ...?

void* malloc(size_t size)
{
    if (!real_malloc) {
        init();
    }

    void* ret = real_malloc(size);

    if (!in_handler) {
        in_handler = true;
        handleMalloc(ret, size);
        in_handler = false;
    }

    return ret;
}

void free(void* ptr)
{
    if (!real_free) {
        init();
    }

    real_free(ptr);

    if (!in_handler) {
        in_handler = true;
        handleFree(ptr);
        in_handler = false;
    }
}

void* realloc(void* ptr, size_t size)
{
    if (!real_realloc) {
        init();
    }

    void* ret = real_realloc(ptr, size);

    if (!in_handler) {
        in_handler = true;
        handleFree(ptr);
        handleMalloc(ret, size);
        in_handler = false;
    }

    return ret;
}

void* calloc(size_t num, size_t size)
{
    if (!real_calloc) {
        init();
    }

    void* ret = real_calloc(num, size);

    if (!in_handler) {
        in_handler = true;
        handleMalloc(ret, num*size);
        in_handler = false;
    }

    return ret;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (!real_posix_memalign) {
        init();
    }

    int ret = real_posix_memalign(memptr, alignment, size);

    if (!in_handler) {
        in_handler = true;
        handleMalloc(*memptr, size);
        in_handler = false;
    }

    return ret;
}

void* aligned_alloc(size_t alignment, size_t size)
{
    if (!real_aligned_alloc) {
        init();
    }

    void* ret = real_aligned_alloc(alignment, size);

    if (!in_handler) {
        in_handler = true;
        handleMalloc(ret, size);
        in_handler = false;
    }

    return ret;
}

void* valloc(size_t size)
{
    if (!real_valloc) {
        init();
    }

    void* ret = real_valloc(size);

    if (!in_handler) {
        in_handler = true;
        handleMalloc(ret, size);
        in_handler = false;
    }

    return ret;
}

}
