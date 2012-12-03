/*
 * Copyright (C) 2012 Intel Corporation
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

/**
 * Including this header file allows to use boost::bind() with
 * a class member as first parameter and a boost::weak_ptr
 * as second parameter.
 *
 * When the functor is invoked, it will lock the instance
 * and only call the member if that succeeds. Otherwise it
 * will silently return to the caller.
 *
 * The member function must have "void" as return value.
 *
 * This behavior makes sense in particular with asynchronous method
 * calls where the result is only relevant while the caller still
 * exists.
 *
 * The code is inspired by
 * http://permalink.gmane.org/gmane.comp.lib.boost.user/70276 and was
 * adapted to the SyncEvolution namespace and coding style. In contrast
 * to that code, the shared_ptr is kept until the invoker gets freed.
 * Seems a bit safer that way. Instead of duplicating the ->* operator
 * in WeakPtrAdapter it uses the type of the member as template parameter
 * to cover all kinds of members.
 */

#ifndef INCL_SYNCEVOLUTION_BOOST_HELPER
# define INCL_SYNCEVOLUTION_BOOST_HELPER

#include <boost/bind.hpp>
#include <boost/smart_ptr.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

template <typename P, typename M>
class WeakPtrInvoker
{
 public:
    WeakPtrInvoker(const P &ptr, const M &member) :
       m_ptr(ptr),
       m_member(member)
    {}

    void operator()() const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)();
        }
    }

    template <typename A1>
    void operator()(A1 a1) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1);
        }
    }

    template <typename A1, typename A2>
    void operator()(A1 a1, A2 a2) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2);
        }
    }

    template <typename A1, typename A2, typename A3>
    void operator()(A1 a1, A2 a2, A3 a3) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2, a3);
        }
    }

    template <typename A1, typename A2, typename A3, typename A4>
    void operator()(A1 a1, A2 a2, A3 a3, A4 a4) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2, a3, a4);
        }
    }

    template <typename A1, typename A2, typename A3, typename A4, typename A5>
    void operator()(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2, a3, a4, a5);
        }
    }

    template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6>
        void operator()(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2, a3, a4, a5, a6);
        }
    }

    template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6, typename A7>
        void operator()(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2, a3, a4, a5, a6, a7);
        }
    }

    template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6, typename A7, typename A8>
        void operator()(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2, a3, a4, a5, a6, a7, a8);
        }
    }

    template <typename A1, typename A2, typename A3, typename A4, typename A5, typename A6, typename A7, typename A8, typename A9>
        void operator()(A1 a1, A2 a2, A3 a3, A4 a4, A5 a5, A6 a6, A7 a7, A8 a8, A9 a9) const
    {
        if (m_ptr) {
            (boost::get_pointer(m_ptr)->*m_member)(a1, a2, a3, a4, a5, a6, a7, a8, a9);
        }
    }

 private:
    P m_ptr;
    M m_member;
};

template <typename T> class WeakPtrAdapter
{
public:
    WeakPtrAdapter(const boost::shared_ptr<T> &ptr) :
        m_ptr(ptr)
    {}

    template <typename M>
    WeakPtrInvoker<boost::shared_ptr<T>, M> operator->*(M member) const
    {
        return WeakPtrInvoker<boost::shared_ptr<T>, M>(m_ptr, member);
    }

private:
    boost::shared_ptr<T> m_ptr;
};

SE_END_CXX

namespace boost
{
    template<class T>
    SyncEvo::WeakPtrAdapter<T> get_pointer(const boost::weak_ptr<T> &ptr)
    {
        return SyncEvo::WeakPtrAdapter<T>(ptr.lock());
    }
}

#endif // INCL_SYNCEVOLUTION_BOOST_HELPER
