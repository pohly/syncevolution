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

#ifndef TIMEOUT_H
#define TIMEOUT_H

#include <glib.h>

#include <syncevo/SmartPtr.h>
#include <syncevo/util.h>

#include <functional>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Utility class which makes it easier to work with g_timeout_add_seconds().
 * Instantiate this class with a specific callback. Use boost::bind()
 * to attach specific parameters to that callback. Then activate
 * the timeout. Destructing this class will automatically remove
 * the timeout and thus ensure that it doesn't trigger without
 * valid parameters.
 *
 * This class is thread-safe. If called by a thread different from the
 * main thread, the callback will happen inside the main thread. Use
 * g_main_context_wakeup() to ensure that the main thread notices the
 * new callback right away.
 */
class Timeout : boost::noncopyable
{
    guint m_tag;
    std::function<bool ()> m_callback;

public:
    enum {
        PRIORITY_HIGH = G_PRIORITY_HIGH,
        PRIORITY_DEFAULT = G_PRIORITY_DEFAULT,
        PRIORITY_HIGH_IDLE = G_PRIORITY_HIGH_IDLE,
        PRIORITY_DEFAULT_IDLE = G_PRIORITY_DEFAULT_IDLE,
        PRIORITY_LOW = G_PRIORITY_LOW
    };

    Timeout() :
        m_tag(0)
    {
    }

    ~Timeout()
    {
        if (m_tag) {
            g_source_remove(m_tag);
        }
    }

    /**
     * call the callback at regular intervals until it returns false
     *
     * @param seconds   a value < 0 runs the function as soon as the process is idle,
     *                  otherwise in the specified amount of time
     */
    template<typename Callback> void activate(int seconds,
                                              Callback &&callback,
                                              int priority = G_PRIORITY_DEFAULT)
    {
        deactivate();

        auto triggered = [] (gpointer data) noexcept {
            Timeout *me = static_cast<Timeout *>(data);
            gboolean runAgain = false;
            uint tag = me->m_tag;
            try {
                // Be extra careful and don't trigger a deactivated callback.
                if (me->m_callback) {
                    runAgain = me->m_callback();
                }
            } catch (...) {
                // Something unexpected went wrong, can only shut down.
                Exception::handle(HANDLE_EXCEPTION_FATAL);
            }
            if (!runAgain && // Returning false will automatically deactivate the source, remember that.
                me->m_tag == tag // Beware that the callback may have already reused the Timeout instance.
                // In that case, we must not reset the new tag and callback.
                ) {
                me->m_tag = 0;
                me->m_callback = 0;
            }
            return runAgain;
        };

        m_callback = std::forward<Callback>(callback);
        m_tag = seconds < 0 ?
            g_idle_add(triggered, static_cast<gpointer>(this)) :
            g_timeout_add_seconds(seconds, triggered, static_cast<gpointer>(this));
        if (!m_tag) {
            SE_THROW("g_timeout_add_seconds() or g_idle_add() failed");
        }
    }

    template<typename Callback> void activate(Callback &&idleCallback,
                                              int priority = G_PRIORITY_DEFAULT_IDLE)
    {
        activate(-1, std::forward<Callback>(idleCallback), priority);
    }

    /**
     * invoke the callback once
     */
    template<typename Callback> void runOnce(int seconds,
                                             Callback &&callback,
                                             int priority = G_PRIORITY_DEFAULT)
    {
        // C++14... would have to use std::bind with C++11.
        auto once = [callback = std::forward<Callback>(callback)] () {
            callback();
            return false;
        };

        activate(seconds, once, priority);
    }
    template<typename Callback> void runOnce(Callback &&idleCallback,
                 int priority = G_PRIORITY_DEFAULT)
    {
        runOnce(-1, std::forward<Callback>(idleCallback), priority);
    }

    /**
     * stop calling the callback, drop callback
     */
    void deactivate()
    {
        if (m_tag) {
            g_source_remove(m_tag);
            m_tag = 0;
        }
        m_callback = 0;
    }

    /** true iff active */
    operator bool () const { return m_tag != 0; }

private:
};

SE_END_CXX

#endif // TIMEOUT_H
