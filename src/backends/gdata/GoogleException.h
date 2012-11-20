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

#ifndef GOOGLE_EXCEPTION_H
#define GOOGLE_EXCEPTION_H

#include <cstring>
#include <exception>


class XGoogle : public std::exception
{
        char *xmsg;

    public:
        XGoogle ()
            { xmsg = 0; }
        XGoogle (const char *message)
            {
                xmsg = new char[strlen(message) + 1];
                strcpy(xmsg, message);
            }
        virtual ~XGoogle () throw ()
            { if (xmsg) delete [] xmsg; }
        const char * what () const throw ()
            { return xmsg; }
};

#endif  // GOOGLE_EXCEPTION_H

