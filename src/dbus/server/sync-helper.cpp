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
        SE_LOG_DEBUG(NULL, NULL, "failure, quitting now: %s",  error.c_str());
        failed = true;
    }

    void onConnect(const DBusConnectionPtr &conn, boost::shared_ptr<SessionHelper> &helper)
    {
        helper.reset(new SessionHelper(loop, conn));
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
        sleep(atoi(delay));
    }

    SyncContext::initMain("syncevo-dbus-helper");

    loop = g_main_loop_new(NULL, FALSE);

    // Suspend and abort are signaled via SIGINT/SIGTERM
    // respectively. SuspendFlags handle that for us.
    SuspendFlags &s = SuspendFlags::getSuspendFlags();
    boost::shared_ptr<SuspendFlags::Guard> guard = s.activate();

    // Redirect both stdout and stderr.
    LogRedirect redirect(true);
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    try {
        if (getenv("SYNCEVOLUTION_DEBUG")) {
            LoggerBase::instance().setLevel(Logger::DEBUG);
        }
        // syncevo-dbus-helper produces the output which is of most
        // interest to users, and therefore it is allowed to print
        // [INFO/ERROR/DEBUG] without including a process name in
        // the brackets, like the other processes do.
        // Logger::setProcessName("syncevo-dbus-helper");

        boost::shared_ptr<ForkExecChild> forkexec = ForkExecChild::create();

        boost::shared_ptr<SessionHelper> helper;
        bool failed = false;
        forkexec->m_onConnect.connect(boost::bind(onConnect, _1, boost::ref(helper)));
        forkexec->m_onFailure.connect(boost::bind(onFailure, _2, boost::ref(failed)));
        forkexec->connect();

        // Run until we are connected, failed or get interrupted.
        // TODO: also quit when parent goes away.
        boost::signals2::connection c =
            s.m_stateChanged.connect(boost::bind(&onAbort));
        SE_LOG_DEBUG(NULL, NULL, "helper (pid %d) finished setup, waiting for parent connection", getpid());
        while (true) {
            if (s.getState() != SuspendFlags::NORMAL) {
                // not an error, someone wanted us to stop
                SE_LOG_INFO(NULL, NULL, "aborted via signal while starting, terminating");
                // tell caller that we aborted by terminating via the SIGTERM signal
                guard.reset();
                raise(SIGTERM);
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
        // Now we no longer care whether the parent connection fails... or do we?
        // TODO: What if the parent fails to call us and instead closes his
        // side of the connection? Will we notice and abort?
        c.disconnect();

        helper->run();

        if (s.getState() == SuspendFlags::ABORT) {
            // not an error, someone wanted us to stop
            SE_LOG_INFO(NULL, NULL, "aborted via signal while running operation, terminating");
            // tell caller that we aborted by terminating via the SIGTERM signal
            guard.reset();
            raise(SIGTERM);
        }

        SE_LOG_DEBUG(NULL, NULL, "terminating normally");
        return 0;
    } catch ( const std::exception &ex ) {
        SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, NULL, "unknown error");
    }

    return 1;
}
