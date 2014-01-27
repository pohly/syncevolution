/*
 * Copyright (C) 2014 Intel Corporation
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

#ifndef INCL_SYNCEVOLUTION_GUARD_FD
# define INCL_SYNCEVOLUTION_GUARD_FD

#include <unistd.h>

#include <boost/noncopyable.hpp>

/**
 * Takes over ownership of a file descriptor and ensures that close()
 * is called on it.
 *
 * To copy it around, use a smart pointer pointing to a GuardFD.
 *
 * Examples:
 * GuardFD fd(open("foo", O_RDONLY));
 * write(fd, ...);
 * return;
 */
class GuardFD : private boost::noncopyable
{
    int m_fd;

 public:
    GuardFD(int fd) : m_fd(fd) {}
    ~GuardFD() throw () { if (m_fd >= 0) close(m_fd); }

    operator int () const { return m_fd; }
    int get() const { return m_fd; }

    int release() { int fd = m_fd; m_fd = -1; return fd; }
    void reset(int fd = -1) { if (m_fd >= 0) close(m_fd); m_fd = fd; }
};

#endif // INCL_SYNCEVOLUTION_GUARD_FD
