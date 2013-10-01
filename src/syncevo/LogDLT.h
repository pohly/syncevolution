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

#ifndef INCL_LOGDLT
#define INCL_LOGDLT

#include <config.h>

#ifdef USE_DLT

#include <syncevo/Logging.h>
#include <syncevo/util.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * A logger which writes to DLT and passes log messages
 * through to its parent.
 */
class LoggerDLT : public Logger
{
    Handle m_parentLogger;
    // avoid dependency on dlt.h here
    void *m_dltContext;

public:
    LoggerDLT(const char *appid, const char *description);
    ~LoggerDLT();

    virtual void messagev(const MessageOptions &options,
                          const char *format,
                          va_list args);

    /**
     * Extracts current log level from the LoggerDLT which was
     * pushed onto the stack, DLT_LOG_DEFAULT if none active.
     */
    static int getCurrentDLTLogLevel();
};

SE_END_CXX

#endif // USE_DLT
#endif // INCL_LOGSYSLOG
