
/*
 * Copyright (C) 2011 Intel Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) version 3.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

#ifndef TIMER_H
#define TIMER_H

#include <unistd.h>
#include <sys/time.h>

#include <syncevo/Timespec.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * A timer helper to check whether now is timeout according to
 * user's setting. Timeout is calculated in milliseconds
 */
class Timer {
    Timespec m_startTime;  ///< start time
    unsigned long m_timeoutMs; ///< timeout in milliseconds, set by user

 public:
    /**
     * constructor
     * @param timeoutMs timeout in milliseconds
     */
    Timer(unsigned long timeoutMs = 0) : m_timeoutMs(timeoutMs)
    {
        reset();
    }

    /**
     * Change the default timeout.
     */
    void setTimeout(unsigned long ms) { m_timeoutMs = ms; }

    /**
     * reset the timer and mark start time as current time
     */
    void reset() { m_startTime.resetMonotonic(); }

    /**
     * check whether it is timeout
     */
    bool timeout()
    {
        return timeout(m_timeoutMs);
    }

    /**
     * check whether the duration timer records is longer than the given duration
     */
    bool timeout(unsigned long timeoutMs)
    {
        return (Timespec::monotonic() - m_startTime).duration() * 1000 >= timeoutMs;
    }
};

SE_END_CXX

#endif // TIMER_H
