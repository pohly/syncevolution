/*
 * Copyright (C) 2013 Intel Corporation
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

#ifndef INCL_THREAD_SUPPORT
# define INCL_THREAD_SUPPORT

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef HAVE_GLIB
# include <glib.h>
#else
# define GLIB_CHECK_VERSION(major, minor, revision) 0
#endif

#include <boost/shared_ptr.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

// The revised threading API we use below was introduced in glib 2.3.2
// The fallback for older glib is to not offer thread support.
// The classes are still defined, they just don't do anything.
#if GLIB_CHECK_VERSION(2, 32, 0)
# define HAVE_THREAD_SUPPORT

/**
 * Core building block for mutices.
 */
template<class M, void (*_lock)(M *), void (*_unlock)(M *)> class MutexTemplate
{
 protected:
    M m_mutex;

 public:
    /**
     * Created when locking the mutex. When the last copy of it
     * gets destroyed, the mutex gets unlocked again.
     */
    class Guard : private boost::shared_ptr<M>
    {
        Guard(M *mutex) throw () :
           boost::shared_ptr<M>(mutex, _unlock)
        {}
        friend class MutexTemplate;

    public:
        Guard() throw()
        {}

        void unlock() throw()
        {
            boost::shared_ptr<M>::reset();
        }
    };

    /**
     * Lock the mutex and return a handle that'll automatically
     * unlock the mutex when the last copy gets destroyed.
     */
    Guard lock() throw ()
    {
        _lock(&m_mutex);
        return Guard(&m_mutex);
    }
};

/**
 * Initializes a mutex which was allocated dynamically
 * on the heap or stack and frees allocated resources
 * when done. It's an error to free a locked mutex.
 */
template<class M, void (*_lock)(M *), void (*_unlock)(M *), void (*_init)(M *), void (*_clear)(M *)> class DynMutexTemplate :
    public MutexTemplate<M, _lock, _unlock>
{
 public:
    DynMutexTemplate()
    {
        _init(&MutexTemplate<M, _lock, _unlock>::m_mutex);
    }
    ~DynMutexTemplate()
    {
        _clear(&MutexTemplate<M, _lock, _unlock>::m_mutex);
    }
};

typedef MutexTemplate<GMutex, g_mutex_lock, g_mutex_unlock> Mutex;
typedef DynMutexTemplate<GMutex, g_mutex_lock, g_mutex_unlock, g_mutex_init, g_mutex_clear> DynMutex;
typedef MutexTemplate<GRecMutex, g_rec_mutex_lock, g_rec_mutex_unlock> RecMutex;
typedef DynMutexTemplate<GRecMutex, g_rec_mutex_lock, g_rec_mutex_unlock, g_rec_mutex_init, g_rec_mutex_clear> DynRecMutex;

#else

# undef HAVE_THREAD_SUPPORT

/**
 * Fallback just to get code compiled.
 */
class DummyMutex
{
 public:
    class Guard
    {
    public:
        void unlock() throw() {}
    };

    Guard lock() throw() { return Guard(); }
};

typedef DummyMutex Mutex;
typedef DummyMutex DynMutex;
typedef DummyMutex RecMutex;
typedef DummyMutex RecDynMutex;

#endif

SE_END_CXX

#endif // INCL_THREAD_SUPPORT
