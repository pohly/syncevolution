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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <iostream>

#include "session.h"
#include "connection.h"

#include <syncevo/SyncContext.h>
#include <syncevo/SuspendFlags.h>
#include <syncevo/ForkExec.h>

#include <boost/bind.hpp>
#include <boost/algorithm/string.hpp>

using namespace SyncEvo;
using namespace GDBusCXX;

namespace {
    GMainLoop *loop = NULL;
    boost::shared_ptr<Session> session;
    boost::shared_ptr<Connection> connection;
}

static void niam(int sig)
{
    SuspendFlags::getSuspendFlags().handleSignal(sig);
    g_main_loop_quit (loop);
}

static void onFailure(const std::string &error)
{
    SE_LOG_INFO(NULL, NULL, "failure, quitting now: %s",  error.c_str());
    g_main_loop_quit(loop);
}

static void onConnect(const DBusConnectionPtr &conn, const std::string &config,
                      const std::string &sessionID, bool startSession)
{
    if(startSession) {
        session = Session::createSession(loop, conn, config, sessionID);
        session->activate();
        SE_LOG_INFO(NULL, NULL, "onConnect called in helper (path: %s interface: %s)",
                    session->getPath(), session->getInterface());
    } else {
        connection.reset(new Connection(loop, conn, sessionID));
        connection->activate();
        SE_LOG_INFO(NULL, NULL, "onConnect called in helper (path: %s interface: %s)",
                    connection->getPath(), connection->getInterface());
    }
}

/**
 * This program is a helper of syncevo-dbus-server which provides the
 * Connection and Session DBus interfaces and runs individual sync
 * sessions. It is only intended to be started by syncevo-dbus-server,
 */
int main(int argc, char **argv, char **envp)
{
    try {
        SyncContext::initMain("syncevo-dbus-helper");

        loop = g_main_loop_new(NULL, FALSE);

        setvbuf(stderr, NULL, _IONBF, 0);
        setvbuf(stdout, NULL, _IONBF, 0);

        signal(SIGTERM, niam);
        signal(SIGINT, niam);

        // LogRedirect redirect(true);

        // make daemon less chatty - long term this should be a command line option
        LoggerBase::instance().setLevel(getenv("SYNCEVOLUTION_DEBUG") ?
                                        LoggerBase::DEBUG :
                                        LoggerBase::INFO);

        boost::shared_ptr<ForkExecChild> forkexec = ForkExecChild::create();

        // Should a Session or a Connection be created?
        bool start_session = forkexec->getEnvVar("SYNCEVO_START_CONNECTION").empty();

        std::string session_config(forkexec->getEnvVar("SYNCEVO_SESSION_CONFIG"));
        std::string session_id(forkexec->getEnvVar("SYNCEVO_SESSION_ID"));
        if(session_id.empty()) {
            return 1;
        }

        SE_LOG_INFO(NULL, NULL, "SYNCEVO_START_CONNECTION = %s in helper", start_session ? "F" : "T");
        SE_LOG_INFO(NULL, NULL, "SYNCEVO_SESSION_CONFIG   = %s in helper", session_config.c_str());
        SE_LOG_INFO(NULL, NULL, "SYNCEVO_SESSION_ID       = %s in helper", session_id.c_str());
        SE_LOG_INFO(NULL, NULL, "SYNCEVOLUTION_FORK_EXEC  = %s in helper", getenv("SYNCEVOLUTION_FORK_EXEC"));

        forkexec->m_onConnect.connect(boost::bind(onConnect, _1, session_config, session_id, start_session));
        forkexec->m_onFailure.connect(boost::bind(onFailure, _2));
        forkexec->connect();

        SE_LOG_INFO(NULL, NULL,
                    "%s: Helper (pid %d) finished setup.",
                    argv[0], getpid());

        g_main_loop_run(loop);

        SE_LOG_INFO(NULL, NULL, "%s: Terminating helper",  argv[0]);
        return 0;
    } catch ( const std::exception &ex ) {
        SE_LOG_ERROR(NULL, NULL, "%s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, NULL, "unknown error");
    }

    return 1;
}
