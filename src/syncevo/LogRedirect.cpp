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

#include "config.h"
#include <syncevo/LogRedirect.h>
#include <syncevo/Logging.h>
#include "test.h"
#include <syncevo/util.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#include <boost/algorithm/string/find_iterator.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/noncopyable.hpp>

#include <algorithm>
#include <iostream>

#ifdef HAVE_GLIB
# include <glib.h>
#endif


#include <syncevo/declarations.h>
SE_BEGIN_CXX

LogRedirect *LogRedirect::m_redirect;
std::set<std::string> LogRedirect::m_knownErrors;

void LogRedirect::abortHandler(int sig) throw()
{
    // Don't know state of logging system, don't log here!
    // SE_LOG_ERROR(NULL, "caught signal %d, shutting down", sig);

    // Shut down redirection, also flushes to log. This involves
    // unsafe calls. For example, we may have to allocate new memory,
    // which deadlocks if glib detected memory corruption and
    // called abort() (see FDO #76375).
    //
    // But flushing the log is the whole point of the abortHandler, so
    // we can't just skip this. To handle cases where the work that we
    // need to do fails, we set a timeout and let the process be
    // killed that way. alarm() and sigaction() are async-signal-safe.
    {
        struct sigaction new_action, old_action;
        memset(&new_action, 0, sizeof(new_action));
        new_action.sa_handler = SIG_DFL; // Terminates the process.
        sigemptyset(&new_action.sa_mask);
        sigaction(SIGALRM, &new_action, &old_action);
        alarm(5);

        RecMutex::Guard guard = lock();
        if (m_redirect) {
            m_redirect->restore();
        }
    }

    // Raise same signal again. Because our handler
    // is automatically removed, this will abort
    // for real now.
    raise(sig);
}

void LogRedirect::init()
{
    m_processing = false;
    m_buffer = nullptr;
    m_len = 0;
    m_out = nullptr;
    m_err = nullptr;
    m_streams = false;
    m_stderr.m_original =
        m_stderr.m_read =
        m_stderr.m_write =
        m_stderr.m_copy = -1;
    m_stdout.m_original =
        m_stdout.m_read =
        m_stdout.m_write =
        m_stdout.m_copy = -1;

    const char *lines = getenv("SYNCEVOLUTION_SUPPRESS_ERRORS");
    if (lines) {
        for (const auto &match: make_iterator_range(boost::make_split_iterator(lines, boost::first_finder("\n", boost::is_iequal())))) {
            m_knownErrors.insert(std::string(match.begin(), match.end()));
        }
    }

    // CONSOLEPRINTF in libsynthesis.
    m_knownErrors.insert("SYSYNC   Rejected with error:");
    // libneon 'Request ends, status 207 class 2xx, error line:'
    m_knownErrors.insert("xx, error line:\n");
    // some internal Qt warning (?)
    m_knownErrors.insert("Qt: Session management error: None of the authentication protocols specified are supported");
}

LogRedirect::LogRedirect(Mode mode, const char *filename)
{
    init();
    m_processing = true;
    if (!getenv("SYNCEVOLUTION_DEBUG")) {
        redirect(STDERR_FILENO, m_stderr);
        if (mode == STDERR_AND_STDOUT) {
            redirect(STDOUT_FILENO, m_stdout);
            m_out = filename ?
                fopen(filename, "w") :
                fdopen(dup(m_stdout.m_copy), "w");
            if (!m_out) {
                restore(m_stdout);
                restore(m_stderr);
                perror(filename ? filename : "LogRedirect fdopen");
            }
        } else if (filename) {
            m_out = fopen(filename, "w");
            if (!m_out) {
                perror(filename);
            }
        }
        // Separate FILE, will write into same file as normal output
        // if a filename was given (for testing), otherwise to original
        // stderr.
        m_err = fdopen(dup((filename && m_out) ?
                           fileno(m_out) :
                           m_stderr.m_copy), "w");
    }

    // Modify process state while holding the Logger mutex.
    RecMutex::Guard guard = lock();
    if (m_redirect) {
        SE_LOG_WARNING(NULL, "LogRedirect already instantiated?!");
    }
    m_redirect = this;

    if (!getenv("SYNCEVOLUTION_DEBUG")) {
        struct sigaction new_action, old_action;
        memset(&new_action, 0, sizeof(new_action));
        new_action.sa_handler = abortHandler;
        sigemptyset(&new_action.sa_mask);
        // disable handler after it was called once
        new_action.sa_flags = SA_RESETHAND;
        // block signals while we handler is active
        // to prevent recursive calls
        sigaddset(&new_action.sa_mask, SIGABRT);
        sigaddset(&new_action.sa_mask, SIGSEGV);
        sigaddset(&new_action.sa_mask, SIGBUS);
        sigaction(SIGABRT, &new_action, &old_action);
        sigaction(SIGSEGV, &new_action, &old_action);
        sigaction(SIGBUS, &new_action, &old_action);
    }
    m_processing = false;
}

LogRedirect::LogRedirect(ExecuteFlags flags)
{
    init();

    // This instance does not modify process state and
    // doesn't have to be thread-safe.
    m_streams = true;
    if (!(flags & EXECUTE_NO_STDERR)) {
        redirect(STDERR_FILENO, m_stderr);
    }
    if (!(flags & EXECUTE_NO_STDOUT)) {
        redirect(STDOUT_FILENO, m_stdout);
    }
}

LogRedirect::~LogRedirect() throw()
{
    RecMutex::Guard guard;
    if (!m_streams) {
        guard = lock();
    }
    if (m_redirect == this) {
        m_redirect = nullptr;
    }
    process();
    restore();
    m_processing = true;
    if (m_out) {
        fclose(m_out);
    }
    if (m_err) {
        fclose(m_err);
    }
    if (m_buffer) {
        free(m_buffer);
    }
}

void LogRedirect::remove() throw()
{
    restore();
}

void LogRedirect::removeRedirect() throw()
{
    if (m_redirect) {
        // We were forked. Ignore mutex (might be held by thread which was not
        // forked) and restore the forked process' state to the one it was
        // before setting up redirection.
        //
        // Do the minimal amount of work possible in restore(), i.e.,
        // suppress the processing of streams.
        m_redirect->m_streams = false;

        m_redirect->restore(m_redirect->m_stdout);
        m_redirect->restore(m_redirect->m_stderr);
    }
}

void LogRedirect::restore() throw()
{
    RecMutex::Guard guard;
    if (!m_streams) {
        guard = lock();
    }

    if (m_processing) {
        return;
    }
    m_processing = true;

    restore(m_stdout);
    restore(m_stderr);

    m_processing = false;
}

void LogRedirect::messagev(const MessageOptions &options,
                           const char *format,
                           va_list args)
{
    RecMutex::Guard guard = lock();

    // check for other output first
    process();
    if (!(options.m_flags & MessageOptions::ONLY_GLOBAL_LOG)) {
        // Choose output channel: SHOW goes to original stdout,
        // everything else to stderr.
        LoggerStdout::write(options.m_level == SHOW ?
                            (m_out ? m_out : stdout) :
                            (m_err ? m_err : stderr),
                            options.m_level, getLevel(),
                            options.m_prefix,
                            options.m_processName,
                            format,
                            args);
    }
}

void LogRedirect::redirect(int original, FDs &fds) throw()
{
    fds.m_original = original;
    fds.m_write = fds.m_read = -1;
    fds.m_copy = dup(fds.m_original);
    if (fds.m_copy >= 0) {
        if (m_streams) {
            // According to Stevens, Unix Network Programming, "Unix
            // domain datagram sockets are similar to UDP sockets: the
            // provide an *unreliable* datagram service that preserves
            // record boundaries." (14.4 Socket Functions,
            // p. 378). But unit tests showed that they do block on
            // Linux and thus seem reliable. Not sure what the official
            // spec is.
            //
            // To avoid the deadlock risk, we must use UDP. But when we
            // want "reliable" behavior *and* detect that all output was
            // processed, we have to use streams despite loosing
            // the write() boundaries, because Unix domain datagram sockets
            // do not flag "end of data".
            int sockets[2];
#define USE_UNIX_DOMAIN_DGRAM 0
            if (!socketpair(AF_LOCAL,
                            USE_UNIX_DOMAIN_DGRAM ? SOCK_DGRAM : SOCK_STREAM,
                            0, sockets)) {
                // success
                fds.m_write = sockets[0];
                fds.m_read = sockets[1];
                return;
            } else {
                perror("LogRedirect::redirect() socketpair");
            }
        } else {
            int write = socket(AF_INET, SOCK_DGRAM, 0);
            if (write >= 0) {
                int read = socket(AF_INET, SOCK_DGRAM, 0);
                if (read >= 0) {
                    struct sockaddr_in addr;
                    memset(&addr, 0, sizeof(addr));
                    addr.sin_family = AF_INET;
                    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    bool bound = false;
                    for (int port = 1025; !bound && port < 10000; port++) {
                        addr.sin_port = htons(port);
                        if (!bind(read, (struct sockaddr *)&addr, sizeof(addr))) {
                            bound = true;
                        }
                    }

                    if (bound) {
                        if (!connect(write, (struct sockaddr *)&addr, sizeof(addr))) {
                            if (dup2(write, fds.m_original) >= 0) {
                                // success
                                fds.m_write = write;
                                fds.m_read = read;
                                return;
                            }
                            perror("LogRedirect::redirect() dup2");
                        }
                        perror("LogRedirect::redirect connect");
                    }
                    close(read);
                }
                close(write);
            }
        }
        close(fds.m_copy);
        fds.m_copy = -1;
    } else {
        perror("LogRedirect::redirect() dup");
    }
}

void LogRedirect::restore(FDs &fds) throw()
{
    if (!m_streams && fds.m_copy >= 0) {
        // flush our own redirected output and process what they might have written
        if (fds.m_original == STDOUT_FILENO) {
            fflush(stdout);
            std::cout << std::flush;
        } else {
            fflush(stderr);
            std::cerr << std::flush;
        }
        process(fds);

        dup2(fds.m_copy, fds.m_original);
    }

    if (fds.m_copy >= 0) {
        close(fds.m_copy);
    }
    if (fds.m_write >= 0) {
        close(fds.m_write);
    }
    if (fds.m_read >= 0) {
        close(fds.m_read);
    }
    fds.m_copy =
        fds.m_write =
        fds.m_read = -1;
}

bool LogRedirect::process(FDs &fds) throw()
{
    bool have_message;
    bool data_read = false;

    if (fds.m_read <= 0) {
        return data_read;
    }

    ssize_t available = 0;
    do {
        have_message = false;

        // keep peeking at the data with increasing buffer sizes until
        // we are sure that we don't truncate it
        size_t newlen = std::max((size_t)1024, m_len);
        while (true) {
            // increase buffer?
            if (newlen > m_len) {
                void *buffer = realloc(m_buffer, newlen);
                if (!buffer) {
                    // Nothing changed.
                    if (available) {
                        // We already read some data of a
                        // datagram. Give up on the rest of the data,
                        // process what we have below.
                        if ((size_t)available == m_len) {
                            // Need the byte for nul termination.
                            available--;
                        }
                        have_message = true;
                        break;
                    } else {
                        // Give up.
                        Exception::throwError(SE_HERE, "out of memory");
                        return false;
                    }
                } else {
                    m_buffer = (char *)buffer;
                    m_len = newlen;
                }
            }
            // read, but leave space for nul byte;
            // when using datagrams, we only peek here and remove the
            // datagram below, without rereading the data
            if (!USE_UNIX_DOMAIN_DGRAM && m_streams) {
                available = recv(fds.m_read, m_buffer, m_len - 1, MSG_DONTWAIT);
                if (available == 0) {
                    return data_read;
                } else if (available == -1) {
                    if (errno == EAGAIN) {
                        // pretend that data was read, so that caller invokes us again
                        return true;
                    } else {
                        Exception::throwError(SE_HERE, "reading output", errno);
                        return false;
                    }
                } else {
                    // data read, process it
                    data_read = true;
                    break;
                }
            } else {
                available = recv(fds.m_read, m_buffer, m_len - 1, MSG_DONTWAIT|MSG_PEEK);
                have_message = available >= 0;
            }
            if (available < (ssize_t)m_len - 1) {
                break;
            } else {
                // try again with twice the buffer
                newlen *= 2;
            }
        }
        if (have_message) {
            if (USE_UNIX_DOMAIN_DGRAM || !m_streams) {
                // swallow packet, even if empty or we couldn't receive it
                recv(fds.m_read, nullptr, 0, MSG_DONTWAIT);
            }
            data_read = true;
        }

        if (available > 0) {
            m_buffer[available] = 0;
            // Now pass it to logger, with a level determined by
            // the channel. This is the point where we can filter
            // out known noise.
            std::string prefix;
            Logger::Level level = Logger::DEV;
            char *text = m_buffer;

            if (fds.m_original == STDOUT_FILENO) {
                // stdout: not sure what this could be, so show it
                level = Logger::SHOW;
                char *eol = strchr(text, '\n');
                if (!m_stdoutData.empty()) {
                    // try to complete previous line, can be done
                    // if text contains a line break
                    if (eol) {
                        m_stdoutData.append(text, eol - text);
                        text = eol + 1;
                        Logger::instance().message(level, prefix.empty() ? nullptr : &prefix,
                                                   nullptr, 0, nullptr,
                                                   "%s", m_stdoutData.c_str());
                        m_stdoutData.clear();
                    }
                }

                // avoid sending incomplete line at end of text,
                // must be done when there is no line break or
                // it is not at the end of the buffer
                eol = strrchr(text, '\n');
                if (eol != m_buffer + available - 1) {
                    if (eol) {
                        m_stdoutData.append(eol + 1);
                        *eol = 0;
                    } else {
                        m_stdoutData.append(text);
                        *text = 0;
                    }
                }

                // output might have been processed as part of m_stdoutData,
                // don't log empty string below
                if (!*text) {
                    continue;
                }
            } else if (fds.m_original == STDERR_FILENO) {
                // stderr: not normally useful for users, so we
                // can filter it more aggressively. For example,
                // swallow extra line breaks, glib inserts those.
                while (*text == '\n') {
                    text++;
                }
                const char *glib_debug_prefix = "** ("; // ** (client-test:875): WARNING **:
                const char *glib_msg_prefix = "** Message:";
                prefix = "stderr";
                if ((!strncmp(text, glib_debug_prefix, strlen(glib_debug_prefix)) &&
                     strstr(text, " **:")) ||
                    !strncmp(text, glib_msg_prefix, strlen(glib_msg_prefix))) {
                    level = Logger::DEBUG;
                    prefix = "glib";
                } else {
                    level = Logger::DEV;
                }

                // If the text contains the word "error", it probably
                // is severe enough to show to the user, regardless of
                // who produced it... except for errors suppressed
                // explicitly.
                if (strcasestr(text, "error") &&
                    !ignoreError(text)) {
                    level = Logger::ERROR;
                }
            }

            // avoid explicit newline at end of output,
            // logging will add it for each message()
            // invocation
            size_t len = strlen(text);
            if (len > 0 && text[len - 1] == '\n') {
                text[len - 1] = 0;
            }
            Logger::instance().message(level, prefix.empty() ? nullptr : &prefix,
                                       nullptr, 0, nullptr,
                                       "%s", text);
            available = 0;
        }
    } while(have_message);

    return data_read;
}

void LogRedirect::addIgnoreError(const std::string &error)
{
    RecMutex::Guard guard = Logger::lock();
    m_knownErrors.insert(error);
}

bool LogRedirect::ignoreError(const std::string &text)
{
    RecMutex::Guard guard = Logger::lock();
    for (const std::string &entry: m_knownErrors) {
        if (text.find(entry) != text.npos) {
            return true;
        }
    }
    return false;
}

void LogRedirect::process()
{
    RecMutex::Guard guard;

    if (m_streams) {
        // iterate until both sockets are closed by peer
        while (true) {
            fd_set readfds;
            fd_set errfds;
            int maxfd = 0;
            FD_ZERO(&readfds);
            FD_ZERO(&errfds);
            if (m_stdout.m_read >= 0) {
                FD_SET(m_stdout.m_read, &readfds);
                FD_SET(m_stdout.m_read, &errfds);
                maxfd = m_stdout.m_read;
            }
            if (m_stderr.m_read >= 0) {
                FD_SET(m_stderr.m_read, &readfds);
                FD_SET(m_stderr.m_read, &errfds);
                if (m_stderr.m_read > maxfd) {
                    maxfd = m_stderr.m_read;
                }
            }
            if (maxfd == 0) {
                // both closed
                return;
            }

            int res = select(maxfd + 1, &readfds, nullptr, &errfds, nullptr);
            switch (res) {
            case -1:
                // fatal, cannot continue
                Exception::throwError(SE_HERE, "waiting for output", errno);
                return;
                break;
            case 0:
                // None ready? Try again.
                break;
            default:
                if (m_stdout.m_read >= 0 && FD_ISSET(m_stdout.m_read, &readfds)) {
                    if (!process(m_stdout)) {
                        // Exact status of a Unix domain datagram socket upon close
                        // of the remote end is a bit uncertain. For TCP, we would end
                        // up here: marked by select as "ready for read", but no data -> EOF.
                        close(m_stdout.m_read);
                        m_stdout.m_read = -1;
                    }
                }
                if (m_stdout.m_read >= 0 && FD_ISSET(m_stdout.m_read, &errfds)) {
                    // But in practice, Unix domain sockets don't mark the stream
                    // as "closed". This is an attempt to detect that situation
                    // via the FDs exception status, but that also doesn't work.
                    close(m_stdout.m_read);
                    m_stdout.m_read = -1;
                }
                if (m_stderr.m_read >= 0 && FD_ISSET(m_stderr.m_read, &readfds)) {
                    if (!process(m_stderr)) {
                        close(m_stderr.m_read);
                        m_stderr.m_read = -1;
                    }
                }
                if (m_stderr.m_read >= 0 && FD_ISSET(m_stderr.m_read, &errfds)) {
                    close(m_stderr.m_read);
                    m_stderr.m_read = -1;
                }
                break;
            }
        }
    } else {
        guard = lock();
    }

    if (m_processing) {
        return;
    }
    m_processing = true;

    process(m_stdout);
    process(m_stderr);

    // avoid hanging onto excessive amounts of memory
    m_len = std::min((size_t)(4 * 1024), m_len);
    m_buffer = (char *)realloc(m_buffer, m_len);
    if (!m_buffer) {
        m_len = 0;
    }

    m_processing = false;
}



void LogRedirect::flush() throw()
{
    RecMutex::Guard guard = lock();

    process();
    if (!m_stdoutData.empty()) {
        std::string buffer;
        std::swap(buffer, m_stdoutData);
        Logger::instance().message(Logger::SHOW, nullptr,
                                   nullptr, 0, nullptr,
                                   "%s", buffer.c_str());
    }
}


#ifdef ENABLE_UNIT_TESTS

class LogRedirectTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(LogRedirectTest);
    CPPUNIT_TEST(simple);
    CPPUNIT_TEST(largeChunk);
    CPPUNIT_TEST(streams);
    CPPUNIT_TEST(overload);
#ifdef HAVE_GLIB
    CPPUNIT_TEST(glib);
#endif
    CPPUNIT_TEST_SUITE_END();

    /**
     * redirect stdout/stderr, then intercept the log messages and
     * store them for inspection
     */
    class LogBuffer : public Logger, private boost::noncopyable
    {
    public:
        std::stringstream m_streams[DEBUG + 1];
        PushLogger<LogRedirect> m_redirect;

        LogBuffer(LogRedirect::Mode mode = LogRedirect::STDERR_AND_STDOUT)
        {
            m_redirect.reset(new LogRedirect(mode));
            addLogger(std::shared_ptr<Logger>(this, NopDestructor()));
        }
        ~LogBuffer()
        {
            removeLogger(this);
            m_redirect.reset();
        }

        virtual void messagev(const MessageOptions &options,
                              const char *format,
                              va_list args)
        {
            CPPUNIT_ASSERT(options.m_level <= DEBUG && options.m_level >= 0);
            m_streams[options.m_level] << StringPrintfV(format, args);
        }
    };
    
public:
    void simple()
    {
        LogBuffer buffer;

        static const char *simpleMessage = "hello world";
        CPPUNIT_ASSERT_EQUAL((ssize_t)strlen(simpleMessage), write(STDOUT_FILENO, simpleMessage, strlen(simpleMessage)));
        buffer.m_redirect->flush();

        CPPUNIT_ASSERT_EQUAL(buffer.m_streams[Logger::SHOW].str(), std::string(simpleMessage));
    }

    void largeChunk()
    {
        LogBuffer buffer;

        std::string large;
        large.append(60 * 1024, 'h');
        CPPUNIT_ASSERT_EQUAL((ssize_t)large.size(), write(STDOUT_FILENO, large.c_str(), large.size()));
        buffer.m_redirect->flush();

        CPPUNIT_ASSERT_EQUAL(large.size(), buffer.m_streams[Logger::SHOW].str().size());
        CPPUNIT_ASSERT_EQUAL(large, buffer.m_streams[Logger::SHOW].str());
    }

    void streams()
    {
        LogBuffer buffer;

        static const char *simpleMessage = "hello world";
        CPPUNIT_ASSERT_EQUAL((ssize_t)strlen(simpleMessage), write(STDOUT_FILENO, simpleMessage, strlen(simpleMessage)));
        static const char *errorMessage = "such a cruel place";
        CPPUNIT_ASSERT_EQUAL((ssize_t)strlen(errorMessage), write(STDERR_FILENO, errorMessage, strlen(errorMessage)));

        // process() keeps unfinished STDOUT lines buffered
        buffer.m_redirect->process();
        CPPUNIT_ASSERT_EQUAL(std::string(errorMessage), buffer.m_streams[Logger::DEV].str());
        CPPUNIT_ASSERT_EQUAL(std::string(""), buffer.m_streams[Logger::SHOW].str());

        // flush() makes them available
        buffer.m_redirect->flush();
        CPPUNIT_ASSERT_EQUAL(std::string(errorMessage), buffer.m_streams[Logger::DEV].str());
        CPPUNIT_ASSERT_EQUAL(std::string(simpleMessage), buffer.m_streams[Logger::SHOW].str());
    }

    void overload()
    {
        LogBuffer buffer;

        std::string large;
        large.append(1024, 'h');
        for (int i = 0; i < 4000; i++) {
            CPPUNIT_ASSERT_EQUAL((ssize_t)large.size(), write(STDOUT_FILENO, large.c_str(), large.size()));
        }
        buffer.m_redirect->flush();

        CPPUNIT_ASSERT(buffer.m_streams[Logger::SHOW].str().size() > large.size());
    }

#ifdef HAVE_GLIB
    void glib()
    {
        fflush(stdout);
        fflush(stderr);

        static const char *filename = "LogRedirectTest_glib.out";
        int new_stdout = open(filename, O_RDWR|O_CREAT|O_TRUNC, S_IRWXU);

        // check that intercept all glib message and don't print anything to stdout
        int orig_stdout = -1;
        try {
            // need to restore the current state below; would be nice
            // to query it instead of assuming that Logger::glogFunc
            // is the current log handler
            g_log_set_default_handler(g_log_default_handler, nullptr);

            orig_stdout = dup(STDOUT_FILENO);
            dup2(new_stdout, STDOUT_FILENO);

            LogBuffer buffer(LogRedirect::STDERR);

            fprintf(stdout, "normal message stdout\n");
            fflush(stdout);

            fprintf(stderr, "normal message stderr\n");
            fflush(stderr);

            // ** (process:13552): WARNING **: test warning
            g_warning("test warning");
            // ** Message: test message
            g_message("test message");
            // ** (process:13552): CRITICAL **: test critical
            g_critical("test critical");
            // would abort:
            // g_error("error")
            // ** (process:13552): DEBUG: test debug
            g_debug("test debug");

            buffer.m_redirect->process();

            std::string error = buffer.m_streams[Logger::ERROR].str();
            std::string warning = buffer.m_streams[Logger::WARNING].str();           
            std::string show = buffer.m_streams[Logger::SHOW].str();
            std::string info = buffer.m_streams[Logger::INFO].str();
            std::string dev = buffer.m_streams[Logger::DEV].str();
            std::string debug = buffer.m_streams[Logger::DEBUG].str();
            CPPUNIT_ASSERT_EQUAL(std::string(""), error);
            CPPUNIT_ASSERT_EQUAL(std::string(""), warning);
            CPPUNIT_ASSERT_EQUAL(std::string(""), show);
            CPPUNIT_ASSERT_EQUAL(std::string(""), info);
            CPPUNIT_ASSERT_EQUAL(std::string(""), error);
            CPPUNIT_ASSERT(dev.find("normal message stderr") != dev.npos);
            CPPUNIT_ASSERT(debug.find("test warning") != debug.npos);
        } catch(...) {
            g_log_set_default_handler(Logger::glogFunc, nullptr);
            dup2(orig_stdout, STDOUT_FILENO);
            throw;
        }
        g_log_set_default_handler(Logger::glogFunc, nullptr);
        dup2(orig_stdout, STDOUT_FILENO);

        lseek(new_stdout, 0, SEEK_SET);
        char out[128];
        ssize_t l = read(new_stdout, out, sizeof(out) - 1);
        CPPUNIT_ASSERT(l > 0);
        out[l] = 0;
        CPPUNIT_ASSERT(boost::starts_with(std::string(out), "normal message stdout"));
    }
#endif
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(LogRedirectTest);

#endif // ENABLE_UNIT_TESTS


SE_END_CXX
