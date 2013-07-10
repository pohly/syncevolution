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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "session-helper.h"

#include <syncevo/Logging.h>
#include <syncevo/ForkExec.h>
#include <syncevo/SuspendFlags.h>
#include <syncevo/SyncContext.h>
#include <syncevo/LogRedirect.h>

using namespace SyncEvo;
using namespace GDBusCXX;

namespace {
    GMainLoop *loop = NULL;

    // that one is actually never called. probably a bug in ForkExec - it should
    // call m_onFailure instead of throwing an exception
    void onFailure(const std::string &error, bool &failed) throw ()
    {
        SE_LOG_DEBUG(NULL, "failure, quitting now: %s",  error.c_str());
        failed = true;
    }

    void onConnect(const DBusConnectionPtr &conn,
                   const boost::shared_ptr<LogRedirect> &parentLogger,
                   const boost::shared_ptr<ForkExecChild> &forkexec,
                   boost::shared_ptr<SessionHelper> &helper)
    {
        helper.reset(new SessionHelper(loop, conn, forkexec, parentLogger));
        helper->activate();
    }

    void onAbort()
    {
        g_main_loop_quit(loop);
    }
} // anonymous namespace

/**
 * This program is a helper of syncevo-dbus-server which provides the
 * Connection and Session DBus interfaces and runs individual sync
 * sessions. It is only intended to be started by syncevo-dbus-server,
 */
int main(int argc, char **argv, char **envp)
{
    // delay the client for debugging purposes
    const char *delay = getenv("SYNCEVOLUTION_LOCAL_CHILD_DELAY");
    if (delay) {
        Sleep(atoi(delay));
    }

    if (getenv("SYNCEVOLUTION_DBUS_HELPER_VGDB")) {
        // Trigger an error in valgrind. Use in combination with
        // --vgdb-error=1 --vgdb=yes (note the =1!) to attach when
        // the process is running.
        void *dummy = malloc(1);
        free(dummy);
        free(dummy);
    }

    SyncContext::initMain("syncevo-dbus-helper");

    loop = g_main_loop_new(NULL, FALSE);

    // Suspend and abort are signaled via SIGINT/SIGTERM
    // respectively. SuspendFlags handle that for us.
    // SIGURG is used as acknowledgement from parent to us that we
    // can quite.
    SuspendFlags &s = SuspendFlags::getSuspendFlags();
    s.setLevel(Logger::DEV);
    boost::shared_ptr<SuspendFlags::Guard> guard = s.activate((1<<SIGINT)|(1<<SIGTERM)|(1<<SIGURG));

    bool debug = getenv("SYNCEVOLUTION_DEBUG");

    // Redirect both stdout and stderr. The only code
    // writing to it should be third-party libraries
    // which are unaware of the SyncEvolution logging system.
    // Redirecting is useful to get such output into our
    // sync logfile, once we have one.
    boost::shared_ptr<LogRedirect> redirect;
    PushLogger<LogRedirect> pushRedirect;
    if (!debug) {
        redirect.reset(new LogRedirect(LogRedirect::STDERR_AND_STDOUT));
        pushRedirect.reset(redirect);
    }
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    try {
        if (debug) {
            Logger::instance().setLevel(Logger::DEBUG);
            Logger::setProcessName(StringPrintf("syncevo-dbus-helper-%ld", (long)getpid()));
        }

        // syncevo-dbus-helper produces the output which is of most
        // interest to users, and therefore it is allowed to print
        // [INFO/ERROR/DEBUG] without including a process name in
        // the brackets, like the other processes do.
        // Logger::setProcessName("syncevo-dbus-helper");

        boost::shared_ptr<ForkExecChild> forkexec = ForkExecChild::create();

        boost::shared_ptr<SessionHelper> helper;
        bool failed = false;
        forkexec->m_onConnect.connect(boost::bind(onConnect, _1, redirect,
                                                  boost::cref(forkexec),
                                                  boost::ref(helper)));
        forkexec->m_onFailure.connect(boost::bind(onFailure, _2, boost::ref(failed)));
        forkexec->connect();

        // Run until we are connected, failed or get interrupted.
        boost::signals2::connection c =
            s.m_stateChanged.connect(boost::bind(&onAbort));
        SE_LOG_DEBUG(NULL, "helper (pid %d) finished setup, waiting for parent connection", getpid());
        while (true) {
            if (s.getState() != SuspendFlags::NORMAL) {
                // not an error, someone wanted us to stop
                SE_LOG_DEBUG(NULL, "aborted via signal while starting, terminating");
                // tell caller that we aborted by terminating via the SIGTERM signal
                return 0;
            }
            if (failed) {
                SE_THROW("parent connection failed");
            }
            if (helper) {
                // done
                break;
            }
            // wait
            g_main_loop_run(loop);
        }
        // Now we no longer care whether the parent connection fails.
        // TODO: What if the parent fails to call us and instead closes his
        // side of the connection? Will we notice and abort?
        c.disconnect();
        SE_LOG_DEBUG(NULL, "connected to parent, run helper");

        helper->run();
        SE_LOG_DEBUG(NULL, "helper operation done");
        helper.reset();
        SE_LOG_DEBUG(NULL, "helper destroyed");

        // Wait for confirmation from parent that we are allowed to
        // quit. This is necessary because we might have pending IO
        // for the parent, like D-Bus method replies.
        while (true) {
            if (s.getReceivedSignals() & (1<<SIGURG)) {
                // not an error, someone wanted us to stop
                SE_LOG_DEBUG(NULL, "aborted via signal after completing operation, terminating");
                return 0;
            }
            if (forkexec->getState() != ForkExecChild::CONNECTED) {
                // No point running any longer, parent is gone.
                //
                // This can occur during normal operations, so don't
                // treat it as an error:
                // - we send final method response
                // - parent signals us and closes the connection
                // - our event loop processes these two events such
                //   that we see the "not connected" one first
                SE_LOG_DEBUG(NULL, "parent has quit, terminating");
                return 0;
            }
            g_main_context_iteration(NULL, true);
        }
    } catch ( const std::exception &ex ) {
        SE_LOG_ERROR(NULL, "helper quitting with exception: %s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, "helper quitting: unknown error");
    }

    return 1;
}
