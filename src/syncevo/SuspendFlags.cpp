/*
 * Copyright (C) 2005-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#include <syncevo/SuspendFlags.h>
#include <syncevo/util.h>
#include <syncevo/ThreadSupport.h>
#include <synthesis/syerror.h>

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>

#include <glib.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static RecMutex suspendRecMutex;

SuspendFlags::SuspendFlags() :
    m_level(Logger::INFO),
    m_state(NORMAL),
    m_receivedSignals(0),
    m_lastSuspend(0),
    m_senderFD(-1),
    m_receiverFD(-1),
    m_activeSignals(0)
{
}

SuspendFlags::~SuspendFlags()
{
    deactivate();
}

SuspendFlags &SuspendFlags::getSuspendFlags()
{
    // never free the instance, other singletons might depend on it
    static SuspendFlags *flags;
    if (!flags) {
        flags = new SuspendFlags;
    }
    return *flags;
}

static gboolean SignalChannelReadyCB(GIOChannel *source,
                                     GIOCondition condition,
                                     gpointer data) throw()
{
    try {
        RecMutex::Guard guard = suspendRecMutex.lock();
        SuspendFlags &me = SuspendFlags::getSuspendFlags();
        me.printSignals();
    } catch (...) {
        Exception::handle();
    }

    return TRUE;
}

/**
 * Own glib IO watch for file descriptor
 * which calls printSignals()
 */
class GLibGuard : public SuspendFlags::Guard
{
    GIOChannel *m_channel;
    guint m_channelReady;

public:
    GLibGuard(int fd)
    {
        // glib watch which calls printSignals()
        m_channel = g_io_channel_unix_new(fd);
        m_channelReady = g_io_add_watch(m_channel, G_IO_IN, SignalChannelReadyCB, NULL);
    }

    ~GLibGuard()
    {
        if (m_channelReady) {
            g_source_remove(m_channelReady);
            m_channelReady = 0;
        }
        if (m_channel) {
            g_io_channel_unref(m_channel);
            m_channel = NULL;
        }
    }
};

SuspendFlags::State SuspendFlags::getState() const {
    RecMutex::Guard guard = suspendRecMutex.lock();
    if (m_abortBlocker.lock()) {
        // active abort blocker
        return ABORT;
    } else if (m_suspendBlocker.lock()) {
        // active suspend blocker
        return SUSPEND;
    } else {
        return m_state;
    }
}

uint32_t SuspendFlags::getReceivedSignals() const {
    RecMutex::Guard guard = suspendRecMutex.lock();
    return m_receivedSignals;
}

Logger::Level SuspendFlags::getLevel() const {
    RecMutex::Guard guard = suspendRecMutex.lock();
    return m_level;
}

void SuspendFlags::setLevel(Logger::Level level) {
    RecMutex::Guard guard = suspendRecMutex.lock();
    m_level = level;
}

bool SuspendFlags::isAborted()
{
    RecMutex::Guard guard = suspendRecMutex.lock();
    printSignals();
    return getState() == ABORT;
}

bool SuspendFlags::isSuspended()
{
    RecMutex::Guard guard = suspendRecMutex.lock();
    printSignals();
    return getState() == SUSPEND;
}

bool SuspendFlags::isNormal()
{
    RecMutex::Guard guard = suspendRecMutex.lock();
    printSignals();
    return getState() == NORMAL;
}

void SuspendFlags::checkForNormal()
{
    RecMutex::Guard guard = suspendRecMutex.lock();
    printSignals();
    if (getState() != NORMAL) {
        SE_THROW_EXCEPTION_STATUS(StatusException,
                                  "aborting as requested by user",
                                  (SyncMLStatus)sysync::LOCERR_USERABORT);
    }
}

boost::shared_ptr<SuspendFlags::StateBlocker> SuspendFlags::suspend() { return block(m_suspendBlocker); }
boost::shared_ptr<SuspendFlags::StateBlocker> SuspendFlags::abort() { return block(m_abortBlocker); }
boost::shared_ptr<SuspendFlags::StateBlocker> SuspendFlags::block(boost::weak_ptr<StateBlocker> &blocker)
{
    RecMutex::Guard guard = suspendRecMutex.lock();
    State oldState = getState();
    boost::shared_ptr<StateBlocker> res = blocker.lock();
    if (!res) {
        res.reset(new StateBlocker);
        blocker = res;
    }
    State newState = getState();
    // only alert receiving side if going from normal -> suspend
    // or suspend -> abort
    if (newState > oldState &&
        m_senderFD >= 0) {
        unsigned char msg = newState;
        // Retry on some errors, ignore others.
        while (write(m_senderFD, &msg, 1) != 1 &&
               (errno == EAGAIN || errno==EINTR))
        {}
    }
    // don't depend on pipes or detecting that change, alert
    // listeners directly
    if (newState != oldState) {
        m_stateChanged(*this);
    }
    return res;
}

boost::shared_ptr<SuspendFlags::Guard> SuspendFlags::activate(uint32_t sigmask)
{
    SE_LOG_DEBUG(NULL, "SuspendFlags: (re)activating, currently %s",
                 m_senderFD > 0 ? "active" : "inactive");
    if (m_senderFD > 0) {
        return m_guard.lock();
    }

    int fds[2];
    if (pipe(fds)) {
        SE_THROW(StringPrintf("allocating pipe for signals failed: %s", strerror(errno)));
    }
    // nonblocking, to avoid deadlocks when the pipe's buffer overflows
    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
    fcntl(fds[1], F_SETFL, fcntl(fds[1], F_GETFL) | O_NONBLOCK);
    m_senderFD = fds[1];
    m_receiverFD = fds[0];
    SE_LOG_DEBUG(NULL, "SuspendFlags: activating signal handler(s) with fds %d->%d",
                 m_senderFD, m_receiverFD);
    for (int sig = 0; sig < 32; sig++) {
        if (sigmask & (1<<sig)) {
            sigaction(sig, NULL, m_oldSignalHandlers + sig);
        }
    }

    struct sigaction new_action;
    memset(&new_action, 0, sizeof(new_action));
    new_action.sa_handler = handleSignal;
    sigemptyset(&new_action.sa_mask);
    // don't let processing of SIGINT be interrupted
    // of SIGTERM and vice versa, if we are doing the
    // handling
    for (int sig = 0; sig < 32; sig++) {
        if (sigmask & (1<<sig)) {
            if (m_oldSignalHandlers[sig].sa_handler == SIG_DFL) {
                sigaddset(&new_action.sa_mask, sig);
            }
        }
    }

    for (int sig = 0; sig < 32; sig++) {
        if (sigmask & (1<<sig)) {
            if (m_oldSignalHandlers[sig].sa_handler == SIG_DFL) {
                sigaction(sig, &new_action, NULL);
                SE_LOG_DEBUG(NULL, "SuspendFlags: catch signal %d", sig);
            }
        }
    }
    m_activeSignals = sigmask;
    boost::shared_ptr<Guard> guard(new GLibGuard(m_receiverFD));
    m_guard = guard;

    return guard;
}

void SuspendFlags::deactivate()
{
    SE_LOG_DEBUG(NULL, "SuspendFlags: deactivating fds %d->%d",
                 m_senderFD, m_receiverFD);
    if (m_receiverFD >= 0) {
        for (int sig = 0; sig < 32; sig++) {
            if (m_activeSignals & (1<<sig)) {
                sigaction(sig, m_oldSignalHandlers + sig, NULL);
            }
        }
        m_activeSignals = 0;
        SE_LOG_DEBUG(NULL, "SuspendFlags: close m_receiverFD %d", m_receiverFD);
        close(m_receiverFD);
        SE_LOG_DEBUG(NULL, "SuspendFlags: close m_senderFD %d", m_senderFD);
        close(m_senderFD);
        m_receiverFD = -1;
        m_senderFD = -1;
        m_guard.reset();
        SE_LOG_DEBUG(NULL, "SuspendFlags: done with deactivation");
    }
}

void SuspendFlags::handleSignal(int sig)
{
    SuspendFlags &me(getSuspendFlags());

    // can't use logging infrastructure in signal handler,
    // not reentrant

    unsigned char msg[2];
    switch (sig) {
    case SIGTERM:
        switch (me.m_state) {
        case ABORT:
            msg[1] = ABORT_AGAIN;
            break;
        default:
            msg[1] = me.m_state = ABORT;
            break;
        }
        break;
    case SIGINT: {
        time_t current;
        time (&current);
        switch (me.m_state) {
        case NORMAL:
            // first time suspend or already aborted
            msg[1] = me.m_state = SUSPEND;
            me.m_lastSuspend = current;
            break;
        case SUSPEND:
            // turn into abort?
            if (current - me.m_lastSuspend < ABORT_INTERVAL) {
                msg[1] = me.m_state = ABORT;
            } else {
                me.m_lastSuspend = current;
                msg[1] = SUSPEND_AGAIN;
            }
            break;
        case ABORT:
            msg[1] = ABORT_AGAIN;
            break;
        case ABORT_AGAIN:
        case SUSPEND_AGAIN:
        case ABORT_MAX:
            // shouldn't happen
            msg[1] = ABORT_MAX;
            break;
        }
    default:
        msg[1] = ABORT_MAX;
        break;
    }
    }
    if (me.m_senderFD >= 0) {
        msg[0] = (unsigned char)(ABORT_MAX + sig);
        size_t left = msg[1] == ABORT_MAX ? 1 : 2;
        while (left) {
            ssize_t written = write(me.m_senderFD, msg, left);
            if (written > 0) {
                left -= written;
            } else {
                if (errno != EAGAIN &&
                    errno != EINTR) {
                    break;
                }
            }
        }
    }
}

void SuspendFlags::printSignals()
{
    RecMutex::Guard guard = suspendRecMutex.lock();
    if (m_receiverFD >= 0) {
        unsigned char msg;
        while (read(m_receiverFD, &msg, 1) == 1) {
            SE_LOG_DEBUG(NULL, "SuspendFlags: read %d from fd %d",
                         msg, m_receiverFD);
            const char *str = NULL;
            switch (msg) {
            case SUSPEND:
                str = "Asking to suspend...\nPress CTRL-C again quickly (within 2s) to stop immediately (can cause problems in the future!)";
                break;
            case SUSPEND_AGAIN:
                str = "Suspend in progress...\nPress CTRL-C again quickly (within 2s) to stop immediately (can cause problems in the future!)";
                break;
            case ABORT:
                str = "Aborting immediately ...";
                break;
            case ABORT_AGAIN:
                str = "Already aborting as requested earlier ...";
                break;
            default: {
                int sig = msg - ABORT_MAX;
                SE_LOG_DEBUG(NULL, "reveived signal %d", sig);
                m_receivedSignals |= 1<<sig;
            }
            }
            if (str) {
                SE_LOG(NULL, m_level, "%s", str);
            }
            m_stateChanged(*this);
        }
    }
}

SE_END_CXX
