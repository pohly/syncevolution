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

#ifndef SESSION_COMMON_H
#define SESSION_COMMON_H

#include "source-status.h"
#include "source-progress.h"
#include "syncevo/util.h"

SE_BEGIN_CXX

/**
 * This class hold constants and defines for Sessions and its
 * consumers.
 */
class SessionCommon
{
public:
    typedef StringMap SourceModes_t;

    enum {
        PRI_CMDLINE    = -10,
        PRI_DEFAULT    =  0,
        PRI_CONNECTION =  10,
        PRI_AUTOSYNC   =  20,
        PRI_SHUTDOWN   =  256  // always higher than anything else
    };

    /**
     * the sync status for session
     */
    enum SyncStatus {
        SYNC_QUEUEING,  ///< waiting to become ready for use
        SYNC_IDLE,      ///< ready, session is initiated but sync not started
        SYNC_RUNNING,   ///< sync is running
        SYNC_ABORT,     ///< sync is aborting
        SYNC_SUSPEND,   ///< sync is suspending
        SYNC_DONE,      ///< sync is done
        SYNC_ILLEGAL
    };

    typedef std::map<std::string, SourceStatus>   SourceStatuses_t;
    typedef std::map<std::string, SourceProgress> SourceProgresses_t;

    /**
     * Number of seconds to wait after file modifications are observed
     * before shutting down or restarting. Shutting down could be done
     * immediately, but restarting might not work right away. 10
     * seconds was chosen because every single package is expected to
     * be upgraded on disk in that interval. If a long-running system
     * upgrade replaces additional packages later, then the server
     * might restart multiple times during a system upgrade. Because it
     * never runs operations directly after starting, that shouldn't
     * be a problem.
     */
    static const int SHUTDOWN_QUIESCENCE_SECONDS = 10;
};

SE_END_CXX

#endif // SESSION_COMMON_H
