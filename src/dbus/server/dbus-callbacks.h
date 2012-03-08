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

#ifndef INCL_DBUS_CALLBACKS
#define INCL_DBUS_CALLBACKS

#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>

#include <syncevo/declarations.h>

SE_BEGIN_CXX

// These callbacks will be used once sync sessions run in their own
// process.
//
// NullCb:    a function that does nothing.
// CounterCb: a function that calls given callback when given counter
//            drops to zero.
//
// Example use:
//
// void allDoneCb() { ... }
// void doAsync(const boost::function<void()> &cb) { ... }
//
// void doIt()
// {
//       boost::shared_ptr<int> counter(new int(1));
//
//   for (...) {
//     ++(*counter);
//     doAsync(boost::bind(&CounterCb, counter, allDoneCb));
//   }
//   // this will run allDoneCb if doAsync was never called.
//   // otherwise it will decrement the counter that was set to 1
//   // at the beginning.
//   CounterCb(counter, allDoneCb);
// }

void NullCb();
void CounterCb(boost::shared_ptr<int> &counter, const boost::function <void()> &callback);

SE_END_CXX

#endif // INCL_DBUS_CALLBACKS
