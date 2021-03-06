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

#if HAS_NOTIFY

#include "notification-backend-libnotify.h"
#include "syncevo/util.h"
#include "syncevo/GLibSupport.h"

#include <stdlib.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <boost/algorithm/string/predicate.hpp>

SE_BEGIN_CXX

NotificationBackendLibnotify::NotificationBackendLibnotify()
    : m_initialized(false),
      m_acceptsActions(false),
      m_notification(nullptr)
{
}

NotificationBackendLibnotify::~NotificationBackendLibnotify()
{
}

void NotificationBackendLibnotify::notifyAction(
    NotifyNotification *notify,
    gchar *action, gpointer userData)
{
    if(boost::iequals(action, "view")) {
        pid_t pid;
        if((pid = fork()) == 0) {
            // search sync-ui from $PATH
            if(execlp("sync-ui", "sync-ui", (const char*)0) < 0) {
                exit(0);
            }
        }
    }
    // if dismissed, ignore.
}

bool NotificationBackendLibnotify::init()
{
    m_initialized = notify_init("SyncEvolution");
    if(m_initialized) {
        GStringListFreeCXX list(notify_get_server_caps());
        for (const char *cap: list) {
            if(boost::iequals(cap, "actions")) {
                m_acceptsActions = true;
            }
        }
        return true;
    }

    return false;
}

void NotificationBackendLibnotify::publish(
    const std::string& summary, const std::string& body,
    const std::string& viewParams)
{
    if(!m_initialized)
        return;

    if(m_notification) {
        notify_notification_clear_actions(m_notification);
        notify_notification_close(m_notification, nullptr);
    }
#ifndef NOTIFY_CHECK_VERSION
# define NOTIFY_CHECK_VERSION(_x,_y,_z) 0
#endif
#if !NOTIFY_CHECK_VERSION(0,7,0)
    m_notification = notify_notification_new(summary.c_str(), body.c_str(), nullptr, nullptr);
#else
    m_notification = notify_notification_new(summary.c_str(), body.c_str(), nullptr);
#endif
    //if actions are not supported, don't add actions
    //An example is Ubuntu Notify OSD. It uses an alert box
    //instead of a bubble when a notification is appended with actions.
    //the alert box won't be closed until user inputs.
    //so disable it in case of no support of actions
    if(m_acceptsActions) {
        notify_notification_add_action(m_notification, "view",
                                       _("View"), notifyAction,
                                       (gpointer)viewParams.c_str(),
                                       nullptr);
        // Use "default" as ID because that is what mutter-moblin
        // recognizes: it then skips the action instead of adding it
        // in addition to its own "Dismiss" button (always added).
        notify_notification_add_action(m_notification, "default",
                                       _("Dismiss"), notifyAction,
                                       (gpointer)viewParams.c_str(),
                                       nullptr);
    }
    notify_notification_show(m_notification, nullptr);
}

SE_END_CXX

#endif // HAS_NOTIFY

