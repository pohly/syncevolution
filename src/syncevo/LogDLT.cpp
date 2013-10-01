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

#include <syncevo/LogDLT.h>

#ifdef USE_DLT

#include <dlt.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static DltLogLevelType SyncEvoLevel2DLTLevel(Logger::Level level)
{
    switch (level) {
    case Logger::NONE: return DLT_LOG_OFF;
    case Logger::ERROR: return DLT_LOG_ERROR;
    case Logger::WARNING: return DLT_LOG_WARN;
    case Logger::SHOW:
    case Logger::INFO: return DLT_LOG_INFO;
    case Logger::DEV:
    case Logger::DEBUG: return DLT_LOG_DEBUG;
    }
    return DLT_LOG_OFF;
}

static LoggerDLT *LoggerDLTInstance;

LoggerDLT::LoggerDLT(const char *appid, const char *description) :
    m_parentLogger(Logger::instance()),
    m_dltContext(calloc(1, sizeof(DltContext)))
{
    DLT_REGISTER_APP(appid, description);
    int level = atoi(getEnv("SYNCEVOLUTION_USE_DLT", "-1"));
    if (level > 0) {
        DLT_REGISTER_CONTEXT_LL_TS(*(DltContext *)m_dltContext, "SYNC", "SyncEvolution messages",
                                   (DltLogLevelType)level, DLT_TRACE_STATUS_OFF);
    } else {
        DLT_REGISTER_CONTEXT(*(DltContext *)m_dltContext, "SYNC", "SyncEvolution messages");
    }
    LoggerDLTInstance = this;
}

LoggerDLT::~LoggerDLT()
{
    DLT_UNREGISTER_CONTEXT(*(DltContext *)m_dltContext);
    DLT_UNREGISTER_APP();
    LoggerDLTInstance = NULL;
}

void LoggerDLT::messagev(const MessageOptions &options,
                         const char *format,
                         va_list args)
{
    // always to parent first (usually stdout):
    // if the parent is a LogRedirect instance, then
    // it'll flush its own output first, which ensures
    // that the new output comes later (as desired)
    {
        va_list argscopy;
        va_copy(argscopy, args);
        m_parentLogger.messagev(options, format, argscopy);
        va_end(argscopy);
    }

    DltContextData log;
    if (!(options.m_flags & MessageOptions::ALREADY_LOGGED) &&
        dlt_user_log_write_start((DltContext *)m_dltContext, &log, SyncEvoLevel2DLTLevel(options.m_level)) > 0) {
        std::string buffer = StringPrintfV(format, args);
        // Avoid almost empty messages. They are triggered by
        // SyncEvolution to format the INFO output and don't add any
        // valuable information to the DLT log.
        if (!buffer.empty() &&
            buffer != "\n") {
            dlt_user_log_write_string(&log, buffer.c_str());
            dlt_user_log_write_finish(&log);
        }
    }
}

int LoggerDLT::getCurrentDLTLogLevel()
{
    if (LoggerDLTInstance) {
        for (int level = DLT_LOG_VERBOSE;
             level > DLT_LOG_DEFAULT;
             --level) {
            DltContextData log;
            // Emulates DLT_LOG(): logging active if dlt_user_log_write_start() returns something > 0.
            // Otherwise discard the DltContextData without doing anything.
            if (dlt_user_log_write_start((DltContext *)LoggerDLTInstance->m_dltContext, &log, (DltLogLevelType)level) > 0) {
                return level;
            }
        }
    }
    return DLT_LOG_DEFAULT;
}

SE_END_CXX

#endif // USE_DLT
