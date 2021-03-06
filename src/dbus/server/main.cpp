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
#include <locale.h>
#include <glib/gi18n.h>

#include "server.h"
#include "restart.h"
#include "session-common.h"

#include <syncevo/SyncContext.h>
#include <syncevo/SuspendFlags.h>
#include <syncevo/LogRedirect.h>
#include <syncevo/LogSyslog.h>
#include <syncevo/LogDLT.h>
#include <syncevo/GLibSupport.h>

#ifdef USE_DLT
# include <dlt.h>
#endif

using namespace SyncEvo;
using namespace GDBusCXX;

namespace {
    GMainLoop *loop = nullptr;
    const char * const execName = "syncevo-dbus-server";

bool parseDuration(int &duration, const char* value)
{
    if(value == nullptr) {
        return false;
    } else if (boost::iequals(value, "unlimited")) {
        duration = -1;
        return true;
    } else if ((duration = atoi(value)) > 0) {
        return true;
    } else {
        return false;
    }
}

} // anonymous namespace

static Logger::Level checkLogLevel(const char *option, int logLevel)
{
    switch (logLevel) {
    case 0: return Logger::NONE;
    case 1: return Logger::ERROR;
    case 2: return Logger::INFO;
    case 3: return Logger::DEBUG;
    default:
        SE_THROW(StringPrintf("invalid parameter value %d for %s: must be one of 0, 1, 2 or 3", logLevel, option));
        return Logger::NONE;
    }
}

int main(int argc, char **argv, char **envp)
{
    // remember environment for restart
    std::shared_ptr<Restart> restart;
    restart.reset(new Restart(argv, envp));

    // Internationalization for auto sync messages.
    setlocale(LC_ALL, "");
    bindtextdomain(GETTEXT_PACKAGE,
                   getEnv("SYNCEVOLUTION_LOCALE_DIR", SYNCEVOLUTION_LOCALEDIR));
    bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
    textdomain(GETTEXT_PACKAGE);

    try {
        gchar *durationString = nullptr;
        int duration = 600;
        int logLevel = 1;
        int logLevelDBus = 2;
        gboolean stdoutEnabled = false;
        gboolean syslogEnabled = true;
#ifdef USE_DLT
        gboolean dltEnabled = false;
#endif
#ifdef ENABLE_DBUS_PIM
        gboolean startPIM = false;
#endif
        GOptionEntry entries[] = {
            { "duration", 'd', 0, G_OPTION_ARG_STRING, &durationString, "Shut down automatically when idle for this duration", "seconds/'unlimited'" },
            { "verbosity", 'v', 0, G_OPTION_ARG_INT, &logLevel,
              "Choose amount of output, 0 = no output, 1 = errors, 2 = info, 3 = debug; default is 1.",
              "level" },
            { "dbus-verbosity", 'v', 0, G_OPTION_ARG_INT, &logLevelDBus,
              "Choose amount of output via D-Bus signals, 0 = no output, 1 = errors, 2 = info, 3 = debug; default is 2.",
              "level" },
            { "stdout", 'o', 0, G_OPTION_ARG_NONE, &stdoutEnabled,
              "Enable printing to stdout (result of operations) and stderr (errors/info/debug).",
              nullptr },
            { "no-syslog", 's', G_OPTION_FLAG_REVERSE, G_OPTION_ARG_NONE, &syslogEnabled, "Disable printing to syslog.", nullptr },
#ifdef USE_DLT
            { "dlt", 0, 0, G_OPTION_ARG_NONE, &dltEnabled, "Enable logging via GENIVI Diagnostic Log and Trace.", nullptr },
#endif
#ifdef ENABLE_DBUS_PIM
            { "start-pim", 'p', 0, G_OPTION_ARG_NONE, &startPIM,
              "Activate the PIM Manager (= unified address book) immediately.",
              nullptr },
#endif
            { nullptr }
        };
        GErrorCXX gerror;
        static GOptionContext *context = g_option_context_new("- SyncEvolution D-Bus Server");
        g_option_context_add_main_entries(context, entries, GETTEXT_PACKAGE);
        bool success = g_option_context_parse(context, &argc, &argv, gerror);
        PlainGStr durationOwner(durationString);
        if (!success) {
            gerror.throwError(SE_HERE, "parsing command line options");
        }
        if (durationString && !parseDuration(duration, durationString)) {
            SE_THROW(StringPrintf("invalid parameter value '%s' for --duration/-d: must be positive number of seconds or 'unlimited'", durationString));
        }
        Logger::Level level = checkLogLevel("--debug", logLevel);
        Logger::Level levelDBus = checkLogLevel("--dbus-debug", logLevelDBus);

        // Temporarily set G_DBUS_DEBUG. Hopefully GIO will read and
        // remember it, because we don't want to keep it set
        // permanently, lest it gets passed on to other processes.
        const char *gdbus = getenv("SYNCEVOLUTION_DBUS_SERVER_GDBUS");
        if (gdbus) {
            setenv("G_DBUS_DEBUG", gdbus, 1);
        }

#ifdef USE_DLT
        PushLogger<LoggerDLT> loggerdlt;
        // set/getenv() are not thread-safe. We set them early to avoid
        // conflicts with threads started by glib, because those tend
        // to call getenv() randomly.
        if (dltEnabled) {
            // DLT logging with default log level DLT_LOG_WARN.  This
            // default was chosen because DLT's own default,
            // DLT_LOG_INFO, leads to too much output given that a lot
            // of the standard messages in SyncEvolution and
            // libsynthesis are labelled informational.
            //
            // SYNCEVOLUTION_USE_DLT and LIBSYNTHESIS_x for x standing for
            // one of the libsynthesis context IDs (list see below) can
            // also be set before invoking SyncEvolution, so here we only
            // set them if unset.
            std::string dlt_value = StringPrintf("%d", DLT_LOG_WARN);
            setenv("SYNCEVOLUTION_USE_DLT", dlt_value.c_str(), false);
            const char *contexts[] = {
                "PROT",
                "SESS",
                "ADMN",
                "DATA",
                "REMI",
                "PARS",
                "GEN",
                "TRNS",
                "SMLT",
                "SYS"
            };
            BOOST_FOREACH (const char *context, contexts) {
                // Help libsynthesis debuglogger.cpp set default log levels,
                // based on our own one.
                SE_LOG_DEBUG(NULL, "default libsynthesis DLT logging of %s = %s",
                             context, dltEnabled);
                setenv((std::string("LIBSYNTHESIS_") + context).c_str(),
                       dlt_value.c_str(),
                       false);
            }
            loggerdlt.reset(new LoggerDLT(DLT_SYNCEVO_DBUS_SERVER_ID, "SyncEvolution D-Bus server"));
        } else {
            unsetenv("SYNCEVOLUTION_USE_DLT");
        }
#endif

        SyncContext::initMain(execName);

        loop = g_main_loop_new (NULL, FALSE);

        setvbuf(stderr, NULL, _IONBF, 0);
        setvbuf(stdout, NULL, _IONBF, 0);

        // Redirect output and optionally log to syslog.
        PushLogger<LogRedirect> redirect(new LogRedirect(LogRedirect::STDERR_AND_STDOUT));
        redirect->setLevel(stdoutEnabled ? level : Logger::NONE);
        PushLogger<LoggerSyslog> syslogger;
        if (syslogEnabled && level > Logger::NONE) {
            syslogger.reset(new LoggerSyslog(execName));
            syslogger->setLevel(level);
        }

        // syncevo-dbus-server should hardly ever produce output that
        // is relevant for end users, so include the somewhat cryptic
        // process name for developers in this process, and not in
        // syncevo-dbus-helper.
        Logger::setProcessName("syncevo-dbus-server");
        std::shared_ptr<SuspendFlags::Guard> guard = SuspendFlags::getSuspendFlags().activate();

        DBusErrorCXX err;
        DBusConnectionPtr conn = dbus_get_bus_connection("SESSION",
                                                         SessionCommon::SERVICE_NAME,
                                                         true,
                                                         &err);
        if (!conn) {
            err.throwFailure("dbus_get_bus_connection()", " failed - server already running?");
        }
        // make this object the main owner of the connection
        auto obj = std::make_unique<DBusObject>(conn, "foo", "bar", true);
        auto server = std::make_shared<SyncEvo::Server>(loop, restart, conn, duration);
        server->setDBusLogLevel(levelDBus);
        server->activate();

#ifdef ENABLE_DBUS_PIM
        std::shared_ptr<GDBusCXX::DBusObjectHelper> manager(SyncEvo::CreateContactManager(server, startPIM));
#endif

        if (gdbus) {
            unsetenv("G_DBUS_DEBUG");
        }

        server->run();
        SE_LOG_DEBUG(NULL, "cleaning up");
#ifdef ENABLE_DBUS_PIM
        manager.reset();
#endif
        server.reset();
        obj.reset();
        SE_LOG_DEBUG(NULL, "flushing D-Bus connection");
        conn.flush();
        conn.reset();
        SE_LOG_INFO(NULL, "terminating, closing logging");
        syslogger.reset();
        redirect.reset();
        guard.reset();
        SE_LOG_INFO(NULL, "terminating");
        return 0;
    } catch ( const std::exception &ex ) {
        SE_LOG_ERROR(NULL, "%s", ex.what());
    } catch (...) {
        SE_LOG_ERROR(NULL, "unknown error");
    }

    return 1;
}
