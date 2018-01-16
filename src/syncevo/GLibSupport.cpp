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

#include <syncevo/GLibSupport.h>
#include <syncevo/Exception.h>
#include <syncevo/SmartPtr.h>
#ifdef ENABLE_UNIT_TESTS
#include "test.h"
#endif

#include <functional>
#include <set>
#include <exception>

#include <string.h>

#ifdef HAVE_GLIB
#include <glib-object.h>
#include <glib.h>
#endif


SE_BEGIN_CXX

#ifdef HAVE_GLIB

class Select {
    GMainLoop *m_loop;
    GMainContext *m_context;
    struct FDSource;
    FDSource *m_source;
    Timespec m_deadline;
    GPollFD m_pollfd;
    GLibSelectResult m_result;

    struct FDSource
    {
        GSource m_source;
        Select *m_select;

        static gboolean prepare(GSource *source,
                                gint *timeout)
        {
            FDSource *fdsource = (FDSource *)source;
            if (!fdsource->m_select->m_deadline) {
                *timeout = -1;
                return FALSE;
            }

            Timespec now = Timespec::monotonic();
            if (now < fdsource->m_select->m_deadline) {
                Timespec delta = fdsource->m_select->m_deadline - now;
                *timeout = delta.tv_sec * 1000 + delta.tv_nsec / 1000000;
                return FALSE;
            } else {
                fdsource->m_select->m_result = GLIB_SELECT_TIMEOUT;
                *timeout = 0;
                return TRUE;
            }
        }

        static gboolean check(GSource *source)
        {
            FDSource *fdsource = (FDSource *)source;
            if (fdsource->m_select->m_pollfd.revents) {
                fdsource->m_select->m_result = GLIB_SELECT_READY;
                return TRUE;
            } else {
                return FALSE;
            }
        }

        static gboolean dispatch(GSource *source,
                                 GSourceFunc callback,
                                 gpointer user_data)
        {
            FDSource *fdsource = (FDSource *)source;
            g_main_loop_quit(fdsource->m_select->m_loop);
            return FALSE;
        }

        static GSourceFuncs m_funcs;
    };

public:
    Select(GMainLoop *loop, int fd, int direction, Timespec *timeout) :
        m_loop(loop),
        m_context(g_main_loop_get_context(loop)),
        m_result(GLIB_SELECT_QUIT)
    {
        if (timeout) {
            m_deadline = Timespec::monotonic() + *timeout;
        }

        memset(&m_pollfd, 0, sizeof(m_pollfd));
        m_source = (FDSource *)g_source_new(&FDSource::m_funcs, sizeof(FDSource));
        if (!m_source) {
            SE_THROW("no FDSource");
        }
        m_source->m_select = this;
        m_pollfd.fd = fd;
        if (fd >= 0 &&
            direction != GLIB_SELECT_NONE) {
            if (direction & GLIB_SELECT_READ) {
                m_pollfd.events |= G_IO_IN | G_IO_HUP | G_IO_ERR;
            }
            if (direction & GLIB_SELECT_WRITE) {
                m_pollfd.events |= G_IO_OUT | G_IO_ERR;
            }
            g_source_add_poll(&m_source->m_source, &m_pollfd);
        }
        g_source_attach(&m_source->m_source, m_context);
    }

    ~Select()
    {
        if (m_source) {
            g_source_destroy(&m_source->m_source);
        }
    }

    GLibSelectResult run()
    {
        g_main_loop_run(m_loop);
        return m_result;
    }
};

GSourceFuncs Select::FDSource::m_funcs = {
    Select::FDSource::prepare,
    Select::FDSource::check,
    Select::FDSource::dispatch
};

GLibSelectResult GLibSelect(GMainLoop *loop, int fd, int direction, Timespec *timeout)
{
    Select instance(loop, fd, direction, timeout);
    return instance.run();
}

void GErrorCXX::throwError(const SourceLocation &where, const std::string &action)
{
    throwError(where, action, m_gerror);
}

void GErrorCXX::throwError(const SourceLocation &where, const std::string &action, const GError *err)
{
    std::string gerrorstr = action;
    if (!gerrorstr.empty()) {
        gerrorstr += ": ";
    }
    if (err) {
        gerrorstr += err->message;
        // No need to clear m_error! Will be done as part of
        // destructing the GErrorCCXX.
    } else {
        gerrorstr = "failure";
    }

    throw Exception(where.m_file, where.m_line, gerrorstr);
}

static void changed(GFileMonitor *monitor,
                    GFile *file1,
                    GFile *file2,
                    GFileMonitorEvent event,
                    gpointer userdata)
{
    GLibNotify::callback_t *callback = static_cast<GLibNotify::callback_t *>(userdata);
    if (*callback) {
        (*callback)(file1, file2, event);
    }
}

GLibNotify::GLibNotify(const char *file, 
                       const callback_t &callback) :
    m_callback(callback)
{
    GFileCXX filecxx(g_file_new_for_path(file), TRANSFER_REF);
    GErrorCXX gerror;
    GFileMonitorCXX monitor(g_file_monitor_file(filecxx.get(), G_FILE_MONITOR_NONE, NULL, gerror), TRANSFER_REF);
    m_monitor.swap(monitor);
    if (!m_monitor) {
        gerror.throwError(SE_HERE, std::string("monitoring ") + file);
    }
    g_signal_connect_after(m_monitor.get(),
                           "changed",
                           G_CALLBACK(changed),
                           (void *)&m_callback);
}

class PendingChecks
{
    typedef std::set<const std::function<bool ()> *> Checks;
    Checks m_checks;
    DynMutex m_mutex;
    Cond m_cond;

public:
    /**
     * Called by main thread before and after sleeping.
     * Runs all registered checks and removes the ones
     * which are done.
     */
    void runChecks();

    /**
     * Called by additional threads. Returns when check()
     * returned false.
     */
    void blockOnCheck(const std::function<bool ()> &check, bool checkFirst);
};

void PendingChecks::runChecks()
{
    DynMutex::Guard guard = m_mutex.lock();
    Checks::iterator it = m_checks.begin();
    bool removed = false;
    while (it != m_checks.end()) {
        bool cont;
        try {
            cont = (**it)();
        } catch (...) {
            Exception::handle(HANDLE_EXCEPTION_FATAL);
            // keep compiler happy
            cont = false;
        }

        if (!cont) {
            // Done with this check
            Checks::iterator next = it;
            ++next;
            m_checks.erase(it);
            it = next;
            removed = true;
        } else {
            ++it;
        }
    }
    // Tell blockOnCheck() calls that they may have completed.
    if (removed) {
        m_cond.signal();
    }
}

void PendingChecks::blockOnCheck(const std::function<bool ()> &check, bool checkFirst)
{
    DynMutex::Guard guard = m_mutex.lock();
    // When we get here, the conditions for returning may already have
    // been met.  Check before sleeping. If we need to continue, then
    // holding the mutex ensures that the main thread will run the
    // check on the next iteration.
    if (!checkFirst || check()) {
        m_checks.insert(&check);
        if (!checkFirst) {
            // Must wake up the main thread from its g_main_context_iteration.
            g_main_context_wakeup(g_main_context_default());
        }
        do {
             m_cond.wait(m_mutex);
        } while (m_checks.find(&check) != m_checks.end());
    }
}

void GRunWhile(const std::function<bool ()> &check, bool checkFirst)
{
    static PendingChecks checks;
    if (g_main_context_is_owner(g_main_context_default())) {
        // Check once before sleeping, conditions may already be met
        // for some checks.
        checks.runChecks();
        // Drive event loop.
        while (check()) {
            g_main_context_iteration(NULL, true);
            checks.runChecks();
        }
    } else {
        // Transfer check into main thread.
        checks.blockOnCheck(check, checkFirst);
    }
}

void GRunInMain(const std::function<void ()> &action)
{
    std::exception_ptr exception;

    // Catch exceptions, then rethrow exception in current thread if there was a problem.
    auto wrapper = [&action, &exception] () mutable noexcept {
        try {
            action();
        } catch (...) {
            exception = std::current_exception();
        }
        // Stop running, action is done.
        return false;
    };
    GRunWhile(wrapper, false);
    if (!exception) {
        std::rethrow_exception(exception);
    }
}

bool GRunIsMain()
{
    // This works because SyncContext::initMain() permanently acquires
    // the main context in the main thread.
    return g_main_context_is_owner(g_main_context_default());
}

#ifdef ENABLE_UNIT_TESTS

class GLibTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(GLibTest);
    CPPUNIT_TEST(notify);
    CPPUNIT_TEST_SUITE_END();

    struct Event {
        GFileCXX m_file1;
        GFileCXX m_file2;
        GFileMonitorEvent m_event;
    };

    static gboolean timeout(gpointer data)
    {
        g_main_loop_quit(static_cast<GMainLoop *>(data));
        return false;
    }

    void notify()
    {
        std::list<Event> events;
        static const char *name = "GLibTest.out";
        unlink(name);
        GMainLoopCXX loop(g_main_loop_new(NULL, FALSE), TRANSFER_REF);
        if (!loop) {
            SE_THROW("could not allocate main loop");
        }
        GLibNotify notify(name,
                          [&events] (GFile *file1,
                                     GFile *file2,
                                     GFileMonitorEvent event) {
                              Event tmp;
                              tmp.m_file1.reset(file1);
                              tmp.m_file2.reset(file2);
                              tmp.m_event = event;
                              events.push_back(tmp);
                          });
        {
            events.clear();
            GLibEvent id(g_timeout_add_seconds(5, timeout, loop.get()), "timeout");
            std::ofstream out(name);
            out << "hello";
            out.close();
            g_main_loop_run(loop.get());
            CPPUNIT_ASSERT(!events.empty());
        }

        {
            events.clear();
            std::ofstream out(name);
            out.close();
            GLibEvent id(g_timeout_add_seconds(5, timeout, loop.get()), "timeout");
            g_main_loop_run(loop.get());
            CPPUNIT_ASSERT(!events.empty());
        }

        {
            events.clear();
            unlink(name);
            GLibEvent id(g_timeout_add_seconds(5, timeout, loop.get()), "timeout");
            g_main_loop_run(loop.get());
            CPPUNIT_ASSERT(!events.empty());
        }
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(GLibTest);

#endif // ENABLE_UNIT_TESTS

#else // HAVE_GLIB

GLibSelectResult GLibSelect(GMainLoop *loop, int fd, int direction, Timespec *timeout)
{
    SE_THROW("GLibSelect() not implemented without glib support");
    return GLIB_SELECT_QUIT;
}

#endif // HAVE_GLIB

SE_END_CXX
