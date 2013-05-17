/*
 * Copyright (C) 2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
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

#include <syncevo/Logging.h>
#include <syncevo/LogStdout.h>
#include <syncevo/LogRedirect.h>

#include <vector>
#include <string.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static RecMutex logMutex;
/**
 * POD to have it initialized without relying on a constructor to run.
 */
static std::string *logProcessName;

void Logger::setProcessName(const std::string &name)
{
    RecMutex::Guard guard = logMutex.lock();
    if (!logProcessName) {
        logProcessName = new std::string(name);
    } else {
        *logProcessName = name;
    }
}

std::string Logger::getProcessName()
{
    RecMutex::Guard guard = logMutex.lock();
    return logProcessName ? *logProcessName : "";
}

RecMutex::Guard Logger::lock()
{
    return logMutex.lock();
}

Logger::Logger() :
    m_level(INFO)
{
}

Logger::~Logger()
{
}

/**
 * Create (if necessary) and return the logger stack.
 * It has at least one entry.
 *
 * logMutex must be locked when calling this.
 */
static std::vector<Logger::Handle> &LoggersSingleton()
{
    // allocate array once and never free it because it might be needed till
    // the very end of the application life cycle
    static std::vector<Logger::Handle> *loggers;
    if (!loggers) {
        loggers = new std::vector<Logger::Handle>;
        // Ensure that the array is never empty.
        boost::shared_ptr<Logger> logger(new LoggerStdout);
        loggers->push_back(logger);
    }
    return *loggers;
}

Logger::Handle Logger::instance()
{
    RecMutex::Guard guard = logMutex.lock();
    std::vector<Handle> &loggers = LoggersSingleton();
    return loggers.back();
}

void Logger::addLogger(const Handle &logger)
{
    RecMutex::Guard guard = logMutex.lock();
    std::vector<Handle> &loggers = LoggersSingleton();

    loggers.push_back(logger);
}

void Logger::removeLogger(Logger *logger)
{
    RecMutex::Guard guard = logMutex.lock();
    std::vector<Handle> &loggers = LoggersSingleton();

    for (ssize_t i = loggers.size() - 1;
         i >= 0;
         --i) {
        if (loggers[i] == logger) {
            loggers[i].remove();
            loggers.erase(loggers.begin() + i);
            break;
        }
    }
}

void Logger::formatLines(Level msglevel,
                             Level outputlevel,
                             const std::string *processName,
                             const std::string *prefix,
                             const char *format,
                             va_list args,
                             boost::function<void (std::string &buffer, size_t expectedTotal)> print)
{
    std::string tag;

    // in case of 'SHOW' level, don't print level and prefix information
    if (msglevel != SHOW) {
        std::string reltime;
        std::string procname;
        std::string firstLine;

        // Run non-blocking operations on shared data while
        // holding the mutex.
        {
            RecMutex::Guard guard = logMutex.lock();
            const std::string *realProcname;

            if (processName) {
                realProcname = processName;
            } else {
                if (!logProcessName) {
                    logProcessName = new std::string;
                }
                realProcname = logProcessName;
            }
            if (!realProcname->empty()) {
                procname.reserve(realProcname->size() + 1);
                procname += " ";
                procname += *realProcname;
            }

            if (outputlevel >= DEBUG) {
                // add relative time stamp
                Timespec now = Timespec::monotonic();
                if (!m_startTime) {
                    // first message, start counting time
                    m_startTime = now;
                    time_t nowt = time(NULL);
                    struct tm tm_gm, tm_local;
                    char buffer[2][80];
                    gmtime_r(&nowt, &tm_gm);
                    localtime_r(&nowt, &tm_local);
                    reltime = " 00:00:00";
                    strftime(buffer[0], sizeof(buffer[0]),
                             "%a %Y-%m-%d %H:%M:%S",
                             &tm_gm);
                    strftime(buffer[1], sizeof(buffer[1]),
                             "%H:%M %z %Z",
                             &tm_local);
                    std::string line =
                        StringPrintf("[DEBUG%s%s] %s UTC = %s\n",
                                     procname.c_str(),
                                     reltime.c_str(),
                                     buffer[0],
                                     buffer[1]);
                } else {
                    if (now >= m_startTime) {
                        Timespec delta = now - m_startTime;
                        reltime = StringPrintf(" %02ld:%02ld:%02ld",
                                               delta.tv_sec / (60 * 60),
                                               (delta.tv_sec % (60 * 60)) / 60,
                                               delta.tv_sec % 60);
                    } else {
                        reltime = " ??:??:??";
                    }
                }
            }
        }

        if (!firstLine.empty()) {
            print(firstLine, 1);
        }
        tag = StringPrintf("[%s%s%s] %s%s",
                           levelToStr(msglevel),
                           procname.c_str(),
                           reltime.c_str(),
                           prefix ? prefix->c_str() : "",
                           prefix ? ": " : "");
    }

    std::string output = StringPrintfV(format, args);

    if (!tag.empty()) {
        // Print individual lines.
        //
        // Total size is guessed by assuming an average line length of
        // around 40 characters to predict number of lines.
        size_t expectedTotal = (output.size() / 40 + 1) * tag.size() + output.size();
        size_t pos = 0;
        while (true) {
            size_t next = output.find('\n', pos);
            if (next != output.npos) {
                std::string line;
                line.reserve(tag.size() + next + 1 - pos);
                line.append(tag);
                line.append(output, pos, next + 1 - pos);
                print(line, expectedTotal);
                pos = next + 1;
            } else {
                break;
            }
        }
        if (pos < output.size() || output.empty()) {
            // handle dangling last line or empty chunk (don't
            // want empty line for that, print at least the tag)
            std::string line;
            line.reserve(tag.size() + output.size() - pos + 1);
            line.append(tag);
            line.append(output, pos, output.size() - pos);
            line += '\n';
            print(line, expectedTotal);
        }
    } else {
        if (!boost::ends_with(output, "\n")) {
            // append newline if necessary
            output += '\n';
        }
        print(output, 0);
    }
}

Logger::MessageOptions::MessageOptions(Level level) :
    m_level(level),
    m_prefix(NULL),
    m_file(NULL),
    m_line(0),
    m_function(NULL),
    m_processName(NULL)
{
}

Logger::MessageOptions::MessageOptions(Level level,
                                       const std::string *prefix,
                                       const char *file,
                                       int line,
                                       const char *function) :
    m_level(level),
    m_prefix(prefix),
    m_file(file),
    m_line(line),
    m_function(function),
    m_processName(NULL)
{
}

Logger::Handle::Handle() throw ()
{
}

Logger::Handle::Handle(Logger *logger) throw ()
{
    m_logger.reset(logger);
}

Logger::Handle::Handle(const Handle &other) throw ()
{
    m_logger = other.m_logger;
}

Logger::Handle & Logger::Handle::operator = (const Handle &other) throw ()
{
    if (this != &other) {
        m_logger = other.m_logger;
    }
    return *this;
}

Logger::Handle::~Handle() throw ()
{
    m_logger.reset();
}

void Logger::Handle::message(Level level,
                             const std::string *prefix,
                             const char *file,
                             int line,
                             const char *function,
                             const char *format,
                             ...)
{
    va_list args;
    va_start(args, format);
    m_logger->messagev(MessageOptions(level, prefix, file, line, function), format, args);
    va_end(args);
}

void Logger::Handle::message(Level level,
                             const std::string &prefix,
                             const char *file,
                             int line,
                             const char *function,
                             const char *format,
                             ...)
{
    va_list args;
    va_start(args, format);
    m_logger->messagev(MessageOptions(level, &prefix, file, line, function), format, args);
    va_end(args);
}

void Logger::Handle::messageWithOptions(const MessageOptions &options,
                                        const char *format,
                                        ...)
{
    va_list args;
    va_start(args, format);
    m_logger->messagev(options, format, args);
    va_end(args);
}

const char *Logger::levelToStr(Level level)
{
    switch (level) {
    case SHOW: return "SHOW";
    case ERROR: return "ERROR";
    case WARNING: return "WARNING";
    case INFO: return "INFO";
    case DEV: return "DEVELOPER";
    case DEBUG: return "DEBUG";
    default: return "???";
    }
}

Logger::Level Logger::strToLevel(const char *str)
{
    // order is based on a rough estimate of message frequency of the
    // corresponding type
    if (!str || !strcmp(str, "DEBUG")) {
        return DEBUG;
    } else if (!strcmp(str, "INFO")) {
        return INFO;
    } else if (!strcmp(str, "SHOW")) {
        return SHOW;
    } else if (!strcmp(str, "ERROR")) {
        return ERROR;
    } else if (!strcmp(str, "WARNING")) {
        return WARNING;
    } else if (!strcmp(str, "DEV")) {
        return DEV;
    } else {
        return DEBUG;
    }
}

#ifdef HAVE_GLIB
void Logger::glogFunc(const gchar *logDomain,
                      GLogLevelFlags logLevel,
                      const gchar *message,
                      gpointer userData)
{
    Level level =
        (logLevel & (G_LOG_LEVEL_ERROR|G_LOG_LEVEL_CRITICAL)) ? ERROR :
        (logLevel & G_LOG_LEVEL_WARNING) ? WARNING :
        (logLevel & (G_LOG_LEVEL_MESSAGE|G_LOG_LEVEL_INFO)) ? SHOW :
        DEBUG;

    // Downgrade some know error messages as registered with
    // the LogRedirect helper class. That messages are registered
    // there is a historic artifact.
    if (level != DEBUG &&
        LogRedirect::ignoreError(message)) {
        level = DEBUG;
    }

    Logger::instance().message(level,
                               NULL,
                               NULL,
                               0,
                               NULL,
                               "%s%s%s",
                               logDomain ? logDomain : "",
                               logDomain ? ": " : "",
                               message);
}

#endif

int Logger::sysyncPrintf(FILE *stream,
                         const char *format,
                         ...)
{
    va_list args;
    va_start(args, format);
    static const std::string prefix("SYSYNC");
    if (boost::starts_with(format, prefix) &&
        format[prefix.size()] == ' ') {
        // Skip initial "SYSYNC " prefix, because it will get re-added
        // in a better way (= to each line) via the prefix parameter.
        format += prefix.size() + 1;
    }
    Logger::instance().messagev(MessageOptions(DEBUG, &prefix, NULL, 0, NULL), format, args);
    va_end(args);

    return 0;
}

SE_END_CXX
