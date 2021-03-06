/*
    author: Ilya Andronov <sni4ok@yandex.ru>
*/

#pragma once

#include "mlog.hpp"

#include <cstdint>

#include <time.h>

#ifndef TTIME_T_DEFINED
struct ttime_t
{
    //value equals (unix_time * 10^9 + nanoseconds)
    static const uint32_t frac = 1000000000;
    uint64_t value;
};
#define TTIME_T_DEFINED
#endif

inline ttime_t get_cur_ttime()
{
    timespec t;
	clock_gettime(CLOCK_REALTIME, &t);

    return ttime_t{uint64_t(t.tv_sec) * ttime_t::frac + t.tv_nsec};
}

inline bool operator>(ttime_t l, ttime_t r)
{
    return l.value > r.value;
}

inline bool operator<(ttime_t l, ttime_t r)
{
    return l.value < r.value;
}

template<typename stream>
stream& operator<<(stream& s, const ttime_t& t)
{
    s << t.value;
    return s;
}

