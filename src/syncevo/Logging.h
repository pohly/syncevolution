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

#ifndef INCL_LOGGING
#define INCL_LOGGING

#include <stdarg.h>
#include <stdio.h>
#include <string>

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif
#ifdef HAVE_GLIB
# include <glib.h>
#endif

#include <syncevo/Timespec.h>
#include <syncevo/ThreadSupport.h>

#include <boost/function.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/noncopyable.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Abstract interface for logging in SyncEvolution.  Can be
 * implemented by other classes to add information (like a certain
 * prefix) before passing the message on to a global instance for the
 * actual processing.
 *
 * The static methods provide some common utility code and manage a
 * global stack of loggers. The one pushed latest is called first to
 * handle a new message. It can find its parent logger (= the one
 * added just before it) and optionally pass the message up the chain
 * before or after processing it itself.
 *
 * All methods must be thread-safe.
 */
class Logger
{
 public:
    /**
     * Which of these levels is the right one for a certain message
     * is a somewhat subjective choice. Here is a definition how they
     * are supposed to be used:
     * - error: severe problem which the user and developer have to
     *          know about
     * - warning: a problem that was handled, but users and developers
     *            probably will want to know about
     * - info: information about a sync session which the user
     *         will want to read during/after each sync session
     * - developer: information about a sync session that is not
     *              interesting for a user (for example, because it
     *              is constant and already known) but which should
     *              be in each log because developers need to know
     *              it. Messages logged with this calls will be included
     *              at LOG_LEVEL_INFO, therefore messages should be small and
     *              not recur so that the log file size remains small.
     * - debug: most detailed logging, messages may be arbitrarily large
     *
     * Here is a decision tree which helps to pick the right level:
     * - an error: => ERROR
     * - a non-fatal error: => WARNING
     * - it changes during each sync or marks important steps
     *   in the sync: INFO
     * - same as before, but without the [INFO] prefix added to each line: => SHOW
     * - small, non-recurring message which is important for developers
     *   who read a log produced at LOG_LEVEL_INFO: DEVELOPER
     * - everything else: DEBUG
     */
    typedef enum {
        /**
         * no error messages printed
         */
        NONE = -1,

        /**
         * only error messages printed
         */
        ERROR,
        /**
         * error and warning messages printed
         */
        WARNING,
        /**
         * "Normal" stdout output which is meant to be seen by a
         * user.
         */
        SHOW,
        /**
         * errors and info messages for users and developers will be
         * printed: use this to keep the output consise and small
         */
        INFO,
        /**
         * important messages to developers
         */
        DEV,
        /**
         * all messages will be printed, including detailed debug
         * messages
         */
        DEBUG
    } Level;
    static const char *levelToStr(Level level);

    /** always returns a valid level, also for NULL, by falling back to DEBUG */
    static Level strToLevel(const char *str);

    /**
     * additional, short string identifying the SyncEvolution process;
     * empty if master process
     *
     * Included by LoggerStdout in the [INFO/DEBUG/...] tag.
     */
    static void setProcessName(const std::string &name);
    static std::string getProcessName();

    /**
     * Obtains the recursive logging mutex.
     *
     * All calls offered by the Logger class already lock that mutex
     * internally, but sometimes it may be necessary to protect a larger
     * region of logging related activity.
     */
    static RecMutex::Guard lock();

#ifdef HAVE_GLIB
    /**
     * can be used in g_log_set_handler() to redirect log messages
     * into our own logging; must be called for each log domain
     * that may be relevant
     */
    static void glogFunc(const gchar *logDomain,
                         GLogLevelFlags logLevel,
                         const gchar *message,
                         gpointer userData);
#endif

    /**
     * can be used as replacement for libsynthesis console printf function,
     * logs at DEBUG level
     *
     * @param stream   is ignored
     * @param format   guaranteed to start with "SYSYNC "
     * @return always 0
     */
    static int sysyncPrintf(FILE *stream,
                            const char *format,
                            ...);

    Logger();
    virtual ~Logger();

    /**
     * Prepare logger for removal from logging stack. May be called
     * multiple times.
     *
     * The logger should stop doing anything right away and just pass
     * on messages until it gets deleted eventually.
     */
    virtual void remove() throw () {}

    /**
     * Collects all the parameters which may get passed to
     * messagev.
     */
    class MessageOptions {
    public:
        /** level for current message */
        Level m_level;
        /** inserted at beginning of each line, if non-NULL */
        const std::string *m_prefix;
        /** source file where message comes from, if non-NULL */
        const char *m_file;
        /** source line number, if file is non-NULL */
        int m_line;
        /** surrounding function name, if non-NULL */
        const char *m_function;
        /** name of the process which originally created the message, if different from current one */
        const std::string *m_processName;
        /** additional flags */
        int m_flags;

        enum {
            /**
             * The message was written into a global log (syslog, dlt, ...)
             * already. Such a message must not be logged again.
             */
            ALREADY_LOGGED = 1<<0,
            /**
             * The message must be written into a global log,
             * but not to stdout.
             */
            ONLY_GLOBAL_LOG = 1<<1
        };

        MessageOptions(Level level);
        MessageOptions(Level level,
                       const std::string *prefix,
                       const char *file,
                       int line,
                       const char *function,
                       int flags = 0);
    };

    /**
     * output a single message
     *
     * @param options   carries additional information about the message
     * @param format    sprintf format
     * @param args      parameters for sprintf: consumed by this function, 
     *                  make copy with va_copy() if necessary!
     */
    virtual void messagev(const MessageOptions &options,
                          const char *format,
                          va_list args) = 0;

    /**
     * A convenience and backwards-compatibility class which allows
     * calling some methods of the underlying pointer directly similar
     * to the Logger reference returned in previous SyncEvolution
     * releases.
     */
    class Handle
    {
        boost::shared_ptr<Logger> m_logger;

    public:
        Handle() throw ();
        Handle(Logger *logger) throw ();
        template<class L> Handle(const boost::shared_ptr<L> &logger) throw () : m_logger(logger) {}
        template<class L> Handle(const boost::weak_ptr<L> &logger) throw () : m_logger(logger.lock()) {}
        Handle(const Handle &other) throw ();
        Handle &operator = (const Handle &other) throw ();
        ~Handle() throw ();

        operator bool () const { return static_cast<bool>(m_logger); }
        bool operator == (Logger *logger) const { return m_logger.get() == logger; }
        Logger *get() const { return m_logger.get(); }

        void messagev(const MessageOptions &options,
                      const char *format,
                      va_list args)
        {
            m_logger->messagev(options, format, args);
        }

        void message(Level level,
                     const std::string *prefix,
                     const char *file,
                     int line,
                     const char *function,
                     const char *format,
                     ...)
#ifdef __GNUC__
            __attribute__((format(printf, 7, 8)))
#endif
            ;
        void message(Level level,
                     const std::string &prefix,
                     const char *file,
                     int line,
                     const char *function,
                     const char *format,
                     ...)
#ifdef __GNUC__
            __attribute__((format(printf, 7, 8)))
#endif
            ;
        void messageWithOptions(const MessageOptions &options,
                                const char *format,
                                ...)
#ifdef __GNUC__
            __attribute__((format(printf, 3, 4)))
#endif
            ;
        void setLevel(Level level) { m_logger->setLevel(level); }
        Level getLevel() { return m_logger->getLevel(); }
        void remove() throw () { m_logger->remove(); }
    };

    /**
     * Grants access to the singleton which implements logging.
     * The implementation of this function and thus the Log
     * class itself is platform specific: if no Log instance
     * has been set yet, then this call has to create one.
     */
    static Handle instance();

    /**
     * Overrides the current default Logger implementation.
     *
     * @param logger    will be used for all future logging activities
     */
    static void addLogger(const Handle &logger);

    /**
     * Remove the specified logger.
     *
     * Note that the logger might still be in use afterwards, for
     * example when a different thread currently uses it. Therefore
     * loggers should be small stub classes. If they need access to
     * more expensive classes to do their work, they shold hold weak
     * reference to those and only lock them when logging.
     */
    static void removeLogger(Logger *logger);

    virtual void setLevel(Level level) { m_level = level; }
    virtual Level getLevel() { return m_level; }

 protected:
    /**
     * Prepares the output. The result is passed back to the caller
     * line-by-line (expectedTotal > 0) and/or as full chunk
     * (expectedTotal = 0). The expected size is just a guess, be
     * prepared to handle more output.
     *
     * Each chunk already includes the necessary line breaks (in
     * particular after the last line when it contains the entire
     * output). It may be modified by the callback.
     *
     * @param processName  NULL means use the current process' name,
     *                     empty means use none
     */
    void formatLines(Level msglevel,
                     Level outputlevel,
                     const std::string *processName,
                     const std::string *prefix,
                     const char *format,
                     va_list args,
                     boost::function<void (std::string &chunk, size_t expectedTotal)> print);

 private:
    Level m_level;

    /**
     * Set by formatLines() before writing the first message if log
     * level is debugging, together with printing a message that gives
     * the local time.
     */
    Timespec m_startTime;
};

/**
 * Takes a logger and adds it to the stack
 * as long as the instance exists.
 */
template<class L> class PushLogger : boost::noncopyable
{
    Logger::Handle m_logger;

 public:
    PushLogger() {}
    /**
     * Can use Handle directly here.
     */
    PushLogger(const Logger::Handle &logger) : m_logger(logger)
    {
        if (m_logger) {
            Logger::addLogger(m_logger);
        }
    }
    /**
     * Take any type that a Handle constructor accepts, then use it as
     * Handle.
     */
    template <class M> PushLogger(M logger) : m_logger(Logger::Handle(logger))
    {
        if (m_logger) {
            Logger::addLogger(m_logger);
        }
    }
    ~PushLogger() throw ()
    {
        if (m_logger) {
            Logger::removeLogger(m_logger.get());
        }
    }

    operator bool () const { return m_logger; }

    void reset(const Logger::Handle &logger)
    {
        if (m_logger) {
            Logger::removeLogger(m_logger.get());
        }
        m_logger = logger;
        if (m_logger) {
            Logger::addLogger(m_logger);
        }
    }
    template<class M> void reset(M logger)
    {
        if (m_logger) {
            Logger::removeLogger(m_logger.get());
        }
        m_logger = Logger::Handle(logger);
        if (m_logger) {
            Logger::addLogger(m_logger);
        }
    }

    void reset()
    {
        if (m_logger) {
            Logger::removeLogger(m_logger.get());
        }
        m_logger = Logger::Handle();
    }

    L *get() { return static_cast<L *>(m_logger.get()); }
    L * operator -> () { return get(); }
};

/**
 * Wraps Logger::message() in the current default logger.
 * and adds file and line where the message comes from.
 *
 * This macro reverses _prefix and _level to avoid the situation where
 * the compiler mistakes a NULL _prefix with the _format parameter
 * (happened once while doing code refactoring).
 *
 * @TODO make source and line info optional for release
 * @TODO add function name (GCC extension)
 */
#define SE_LOG(_prefix, _level, _format, _args...) \
    SyncEvo::Logger::instance().message(_level, \
                                        _prefix, \
                                        __FILE__, \
                                        __LINE__, \
                                        NULL, \
                                        _format, \
                                        ##_args);

#define SE_LOG_SHOW(_prefix, _format, _args...) SE_LOG(_prefix, SyncEvo::Logger::SHOW, _format, ##_args)
#define SE_LOG_ERROR(_prefix, _format, _args...) SE_LOG(_prefix, SyncEvo::Logger::ERROR, _format, ##_args)
#define SE_LOG_WARNING(_prefix, _format, _args...) SE_LOG(_prefix, SyncEvo::Logger::WARNING, _format, ##_args)
#define SE_LOG_INFO(_prefix, _format, _args...) SE_LOG(_prefix, SyncEvo::Logger::INFO, _format, ##_args)
#define SE_LOG_DEV(_prefix, _format, _args...) SE_LOG(_prefix, SyncEvo::Logger::DEV, _format, ##_args)
#define SE_LOG_DEBUG(_prefix, _format, _args...) SE_LOG(_prefix, SyncEvo::Logger::DEBUG, _format, ##_args)
 
SE_END_CXX
#endif // INCL_LOGGING
