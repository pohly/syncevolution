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

#include <memory>

#include "config.h"

#ifdef ENABLE_ECAL

// include first, it sets HANDLE_LIBICAL_MEMORY for us
#include <syncevo/icalstrdup.h>

#include <syncevo/Exception.h>
#include <syncevo/SmartPtr.h>
#include <syncevo/Logging.h>

#include "EvolutionCalendarSource.h"
#include "EvolutionMemoSource.h"
#include "e-cal-check-timezones.h"

#include <syncevo/declarations.h>
SE_BEGIN_CXX

static const string
EVOLUTION_CALENDAR_PRODID("PRODID:-//ACME//NONSGML SyncEvolution//EN"),
EVOLUTION_CALENDAR_VERSION("VERSION:2.0");

// CalComponent is a smart pointer which wraps either an icalcomponent (libecal < 2.0)
// or a ICalComponent (libical >= 2.0). For ICalComponent, it always owns
// the object. For ICalComponent, that is the default, but can be overridden.
class CalComponent : private boost::noncopyable {
public:
#ifdef HAVE_LIBECAL_2_0
    typedef ICalComponent T;
#else
    typedef icalcomponent T;
#endif

    CalComponent(T *component = nullptr, bool owned = true) : m_component(component), m_owned(owned) {}
    ~CalComponent() { free(); }
    CalComponent & operator = (T *component) {
        free();
        m_component = component;
        return *this;
    }
    operator T * () { return m_component; }
    operator bool () { return m_component != 0; }
    T * Steal() { T *component = m_component; m_component = nullptr; return component; }

#ifdef HAVE_LIBECAL_2_0
    static T *new_from_string(const char *str) { return i_cal_component_new_from_string(str); }
    static const auto VTIMEZONE_COMPONENT = I_CAL_VTIMEZONE_COMPONENT;
    static T *get_first_component(T *comp, ICalComponentKind what) { return i_cal_component_get_first_component(comp, what); }
    static T *get_next_component(T *comp, ICalComponentKind what) { return i_cal_component_get_next_component(comp, what); }
#else
    static T *new_from_string(const char *str) { return icalcomponent_new_from_string(str); }
    static const auto VTIMEZONE_COMPONENT = ICAL_VTIMEZONE_COMPONENT;
    static T *get_first_component(T *comp, icalcomponent_kind what) { return icalcomponent_get_first_component(comp, what); }
    static T *get_next_component(T *comp, icalcomponent_kind what) { return icalcomponent_get_next_component(comp, what); }
#endif

private:
    T *m_component;
    bool m_owned;

    void free() {
        if (m_component) {
#ifdef HAVE_LIBECAL_2_0
            g_object_unref(m_component);
#else
            if (m_owned) {
                icalcomponent_free(m_component);
            }
#endif
        }
    }
};

#ifdef HAVE_LIBECAL_2_0
typedef ICalTimezone CalTimezone;
#else
typedef icaltimezone CalTimezone;
#endif

bool EvolutionCalendarSource::LUIDs::containsLUID(const ItemID &id) const
{
    const_iterator it = findUID(id.m_uid);
    return it != end() &&
        it->second.find(id.m_rid) != it->second.end();
}

void EvolutionCalendarSource::LUIDs::insertLUID(const ItemID &id)
{
    (*this)[id.m_uid].insert(id.m_rid);
}

void EvolutionCalendarSource::LUIDs::eraseLUID(const ItemID &id)
{
    iterator it = find(id.m_uid);
    if (it != end()) {
        auto it2 = it->second.find(id.m_rid);
        if (it2 != it->second.end()) {
            it->second.erase(it2);
            if (it->second.empty()) {
                erase(it);
            }
        }
    }
}

static int granularity()
{
    // This long delay is necessary in combination
    // with Evolution Exchange Connector: when updating
    // a child event, it seems to take a while until
    // the change really is effective.
    static int secs = 5;
    static bool checked = false;
    if (!checked) {
        // allow setting the delay (used during testing to shorten runtime)
        const char *delay = getenv("SYNC_EVOLUTION_EVO_CALENDAR_DELAY");
        if (delay) {
            secs = atoi(delay);
        }
        checked = true;
    }
    return secs;
}

EvolutionCalendarSource::EvolutionCalendarSource(EvolutionCalendarSourceType type,
                                                 const SyncSourceParams &params) :
    EvolutionSyncSource(params, granularity()),
    m_type(type)
{
    switch (m_type) {
     case EVOLUTION_CAL_SOURCE_TYPE_EVENTS:
        SyncSourceLogging::init(InitList<std::string>("SUMMARY") + "LOCATION",
                                ", ",
                                m_operations);
        m_typeName = "calendar";
#ifndef USE_EDS_CLIENT
        m_newSystem = e_cal_new_system_calendar;
#endif
        break;
     case EVOLUTION_CAL_SOURCE_TYPE_TASKS:
        SyncSourceLogging::init(InitList<std::string>("SUMMARY"),
                                ", ",
                                m_operations);
        m_typeName = "task list";
#ifndef USE_EDS_CLIENT
        m_newSystem = e_cal_new_system_tasks;
#endif
        break;
     case EVOLUTION_CAL_SOURCE_TYPE_MEMOS:
        SyncSourceLogging::init(InitList<std::string>("SUBJECT"),
                                ", ",
                                m_operations);
        m_typeName = "memo list";
#ifndef USE_EDS_CLIENT
        // This is not available in older Evolution versions.
        // A configure check could detect that, but as this isn't
        // important the functionality is simply disabled.
        m_newSystem = nullptr /* e_cal_new_system_memos */;
#endif
        break;
     default:
        Exception::throwError(SE_HERE, "internal error, invalid calendar type");
        break;
    }
}

SyncSource::Databases EvolutionCalendarSource::getDatabases()
{
    GErrorCXX gerror;
    Databases result;

#ifdef USE_EDS_CLIENT
    getDatabasesFromRegistry(result,
                             sourceExtension(),
                             m_type == EVOLUTION_CAL_SOURCE_TYPE_EVENTS ? e_source_registry_ref_default_calendar :
                             m_type == EVOLUTION_CAL_SOURCE_TYPE_TASKS ? e_source_registry_ref_default_task_list :
                             m_type == EVOLUTION_CAL_SOURCE_TYPE_MEMOS ? e_source_registry_ref_default_memo_list :
                             nullptr);
#else
    ESourceList *tmp = nullptr;
    if (!e_cal_get_sources(&tmp, sourceType(), gerror)) {
        // ignore unspecific errors (like on Maemo with no support for memos)
        // and continue with empty list (perhaps defaults work)
        if (!gerror) {
            tmp = nullptr;
        } else {
            throwError(SE_HERE, "unable to access backend databases", gerror);
        }
    }
    ESourceListCXX sources(tmp, TRANSFER_REF);
    bool first = true;
    for (GSList *g = sources ? e_source_list_peek_groups (sources) : nullptr;
         g;
         g = g->next) {
        ESourceGroup *group = E_SOURCE_GROUP (g->data);
        for (GSList *s = e_source_group_peek_sources (group); s; s = s->next) {
            ESource *source = E_SOURCE (s->data);
            eptr<char> uri(e_source_get_uri(source));
            result.push_back(Database(e_source_peek_name(source),
                                      uri ? uri.get() : "",
                                      first));
            first = false;
        }
    }
    if (result.empty() && m_newSystem) {
        eptr<ECal, GObject> calendar(m_newSystem());
        if (calendar.get()) {
            // okay, default system database exists
            const char *uri = e_cal_get_uri(calendar.get());
            result.push_back(Database("<<system>>", uri ? uri : "<<unknown uri>>"));
        }
    }
#endif

    return result;
}

#ifndef USE_EDS_CLIENT
char *EvolutionCalendarSource::authenticate(const char *prompt,
                                            const char *key)
{
    std::string passwd = getPassword();

    SE_LOG_DEBUG(getDisplayName(), "authentication requested, prompt \"%s\", key \"%s\" => %s",
                 prompt, key,
                 !passwd.empty() ? "returning configured password" : "no password configured");
    return !passwd.empty() ? strdup(passwd.c_str()) : nullptr;
}
#endif



void EvolutionCalendarSource::open()
{
#ifdef USE_EDS_CLIENT
    // Open twice. This solves an issue where Evolution's CalDAV
    // backend only updates its local cache *after* a sync (= while
    // closing the calendar?), instead of doing it *before* a sync (in
    // e_cal_open()).
    //
    // This workaround is applied to *all* backends because there might
    // be others with similar problems and for local storage it is
    // a reasonably cheap operation (so no harm there).
    for (int retries = 0; retries < 2; retries++) {
        auto create = [type=sourceType()] (ESource *source, GError **gerror) {
            return E_CLIENT(e_cal_client_connect_sync(source, type,
                                                      -1, // timeout in seconds
                                                      nullptr, // cancellable
                                                      gerror));
        };
        m_calendar.reset(E_CAL_CLIENT(openESource(sourceExtension(),
                                                  m_type == EVOLUTION_CAL_SOURCE_TYPE_EVENTS ? e_source_registry_ref_builtin_calendar :
                                                  m_type == EVOLUTION_CAL_SOURCE_TYPE_TASKS ? e_source_registry_ref_builtin_task_list :
                                                  m_type == EVOLUTION_CAL_SOURCE_TYPE_MEMOS ? e_source_registry_ref_builtin_memo_list :
                                                  nullptr,
                                                  create).get()));
    }
#else
    GErrorCXX gerror;
    bool onlyIfExists = false; // always try to create address book, because even if there is
                               // a source there's no guarantee that the actual database was
                               // created already; the original logic below for only setting
                               // this when explicitly requesting a new database
                               // therefore failed in some cases

    ESourceList *tmp;
    if (!e_cal_get_sources(&tmp, sourceType(), gerror)) {
        throwError(SE_HERE, "unable to access backend databases", gerror);
    }
    ESourceListCXX sources(tmp, TRANSFER_REF);

    string id = getDatabaseID();    
    ESource *source = findSource(sources, id);
    bool created = false;

    // Open twice. This solves an issue where Evolution's CalDAV
    // backend only updates its local cache *after* a sync (= while
    // closing the calendar?), instead of doing it *before* a sync (in
    // e_cal_open()).
    //
    // This workaround is applied to *all* backends because there might
    // be others with similar problems and for local storage it is
    // a reasonably cheap operation (so no harm there).
    for (int retries = 0; retries < 2; retries++) {
        if (!source) {
            // might have been special "<<system>>" or "<<default>>", try that and
            // creating address book from file:// URI before giving up
            if ((id.empty() || id == "<<system>>") && m_newSystem) {
                m_calendar.set(m_newSystem(), (string("system ") + m_typeName).c_str());
            } else if (!id.compare(0, 7, "file://")) {
                m_calendar.set(e_cal_new_from_uri(id.c_str(), sourceType()), (string("creating ") + m_typeName).c_str());
            } else {
                throwError(SE_HERE, string("not found: '") + id + "'");
            }
            created = true;
            onlyIfExists = false;
        } else {
            m_calendar.set(e_cal_new(source, sourceType()), m_typeName.c_str());
        }

        e_cal_set_auth_func(m_calendar, eCalAuthFunc, this);
    
        if (!e_cal_open(m_calendar, onlyIfExists, gerror)) {
            if (created) {
                // opening newly created address books often failed, perhaps that also applies to calendars - try again
                gerror.clear();
                sleep(5);
                if (!e_cal_open(m_calendar, onlyIfExists, gerror)) {
                    throwError(SE_HERE, string("opening ") + m_typeName, gerror);
                }
            } else {
                throwError(SE_HERE, string("opening ") + m_typeName, gerror);
            }
        }
    }

#endif

    g_signal_connect_after(m_calendar,
                           "backend-died",
                           G_CALLBACK(Exception::fatalError),
                           (void *)"Evolution Data Server has died unexpectedly, database no longer available.");
}

bool EvolutionCalendarSource::isEmpty()
{
    // TODO: add more efficient implementation which does not
    // depend on actually pulling all items from EDS
    RevisionMap_t revisions;
    listAllItems(revisions);
    return revisions.empty();
}

#ifdef USE_EDS_CLIENT
class ECalClientViewSyncHandler {
  public:
    typedef std::function<void(const GSList *list)> Process_t;

    ECalClientViewSyncHandler(ECalClientViewCXX &view,
                              const Process_t &process) :
        m_process(process),
        m_view(view)
    {}

    bool processSync(GErrorCXX &gerror)
    {
        // Listen for view signals
        m_view.connectSignal<ECalClientView *,
                             const GSList *>()("objects-added",
                                               [this] (ECalClientView *, const GSList *list) { m_process(list); });
        m_view.connectSignal<ECalClientView *,
                             const GError *>()("complete",
                                               [this] (ECalClientView *, const GError *gerror) { completed(gerror); });

        // Start the view
        e_cal_client_view_start (m_view, m_error);
        if (m_error) {
            std::swap(gerror, m_error);
            return false;
        }

        // Async -> Sync
        m_loop.run();
        e_cal_client_view_stop (m_view, nullptr);

        if (m_error) {
            std::swap(gerror, m_error);
            return false;
        } else {
            return true;
        }
    }

    void completed(const GError *error)
    {
        m_error = error;
        m_loop.quit();
    }

    public:
      // Event loop for Async -> Sync
      EvolutionAsync m_loop;

    private:
      // Process list callback
      Process_t m_process;

      // View watched
      ECalClientViewCXX m_view;

      // Possible error while watching the view
      GErrorCXX m_error;
};
#endif // USE_EDS_CLIENT

void EvolutionCalendarSource::listAllItems(RevisionMap_t &revisions)
{
    GErrorCXX gerror;
#ifdef USE_EDS_CLIENT
    ECalClientView *view;

    if (!e_cal_client_get_view_sync (m_calendar, "#t", &view, nullptr, gerror)) {
        throwError(SE_HERE, "getting the view" , gerror);
    }
    ECalClientViewCXX viewPtr = ECalClientViewCXX::steal(view);

    // TODO: Optimization: use set fields_of_interest (UID / REV / LAST-MODIFIED)

    auto process = [&revisions] (const GSList *objects) {
        const GSList *l;

        for (l = objects; l; l = l->next) {
            CalComponent icomp((CalComponent::T *)l->data, false);
            EvolutionCalendarSource::ItemID id = EvolutionCalendarSource::getItemID(icomp);
            string luid = id.getLUID();
            string modTime = EvolutionCalendarSource::getItemModTime(icomp);
            revisions[luid] = modTime;
        }
    };
    ECalClientViewSyncHandler handler(viewPtr, process);
    if (!handler.processSync(gerror)) {
        throwError(SE_HERE, "watching view", gerror);
    }

    // Update m_allLUIDs
    m_allLUIDs.clear();
    RevisionMap_t::iterator it;
    for(it = revisions.begin(); it != revisions.end(); ++it) {
        m_allLUIDs.insertLUID(it->first);
    }
#else
    GList *nextItem;

    m_allLUIDs.clear();
    if (!e_cal_get_object_list_as_comp(m_calendar,
                                       "#t",
                                       &nextItem,
                                       gerror)) {
        throwError(SE_HERE, "reading all items", gerror);
    }
    eptr<GList> listptr(nextItem);
    while (nextItem) {
        ECalComponent *ecomp = E_CAL_COMPONENT(nextItem->data);
        ItemID id = getItemID(ecomp);
        string luid = id.getLUID();
        string modTime = getItemModTime(ecomp);

        m_allLUIDs.insertLUID(id);
        revisions[luid] = modTime;
        nextItem = nextItem->next;
    }
#endif
}

void EvolutionCalendarSource::close()
{
    m_calendar.reset();
}

void EvolutionCalendarSource::readItem(const string &luid, std::string &item, bool raw)
{
    ItemID id(luid);
    item = retrieveItemAsString(id);
}

#ifdef USE_EDS_CLIENT
CalTimezone *
my_tzlookup(const gchar *tzid,
#ifdef HAVE_LIBECAL_2_0
            gpointer ecalclient,
#else
            gconstpointer ecalclient,
#endif
            GCancellable *cancellable,
            GError **error)
{
    CalTimezone *zone = nullptr;
    GError *local_error = nullptr;

    if (e_cal_client_get_timezone_sync((ECalClient *)ecalclient, tzid, &zone, cancellable, &local_error)) {
        return zone;
    } else if (local_error && local_error->domain == E_CAL_CLIENT_ERROR) {
        // Ignore *all* E_CAL_CLIENT_ERROR errors, e_cal_client_get_timezone_sync() does
        // not reliably return a specific code like E_CAL_CLIENT_ERROR_OBJECT_NOT_FOUND.
        // See the 'e_cal_client_check_timezones() + e_cal_client_tzlookup() + Could not retrieve calendar time zone: Invalid object'
        // mail thread.
        g_clear_error (&local_error);
    } else if (local_error) {
        g_propagate_error (error, local_error);
    }

    return nullptr;
}
#endif

EvolutionCalendarSource::InsertItemResult EvolutionCalendarSource::insertItem(const string &luid, const std::string &item, bool raw)
{
    bool update = !luid.empty();
    InsertItemResultState state = ITEM_OKAY;
    bool detached = false;
    string newluid = luid;
    string data = item;
    string modTime;

    /*
     * Evolution/libical can only deal with \, as separator.
     * Replace plain , in incoming event CATEGORIES with \, -
     * based on simple text search/replace and thus will not work
     * in all cases...
     *
     * Inverse operation in extractItemAsString().
     */
    size_t propstart = data.find("\nCATEGORIES");
    bool modified = false;
    while (propstart != data.npos) {
        size_t eol = data.find('\n', propstart + 1);
        size_t comma = data.find(',', propstart);

        while (eol != data.npos &&
               comma != data.npos &&
               comma < eol) {
            if (data[comma-1] != '\\') {
                data.insert(comma, "\\");
                comma++;
                modified = true;
            }
            comma = data.find(',', comma + 1);
        }
        propstart = data.find("\nCATEGORIES", propstart + 1);
    }
    if (modified) {
        SE_LOG_DEBUG(getDisplayName(), "after replacing , with \\, in CATEGORIES:\n%s", data.c_str());
    }

    eptr<CalComponent::T> icomp(CalComponent::new_from_string((char *)data.c_str()));

    if( !icomp ) {
        throwError(SE_HERE, string("failure parsing ical") + data);
    }

    GErrorCXX gerror;

    // fix up TZIDs
    if (
#ifdef USE_EDS_CLIENT
#ifdef HAVE_LIBECAL_2_0
        !e_cal_client_check_timezones_sync(
#else
        !e_cal_client_check_timezones(
#endif
                                      icomp,
                                      nullptr,
                                      my_tzlookup,
#ifdef HAVE_LIBECAL_2_0
                                      (gpointer)m_calendar.get(),
#else
                                      (const void *)m_calendar.get(),
#endif
                                      nullptr,
                                      gerror)
#else
        !e_cal_check_timezones(icomp,
                               nullptr,
                               e_cal_tzlookup_ecal,
                               (const void *)m_calendar.get(),
                               gerror)
#endif
        ) {
        throwError(SE_HERE, string("fixing timezones") + data,
                   gerror);
    }

    // insert before adding/updating the event so that the new VTIMEZONE is
    // immediately available should anyone want it
    for (CalComponent tcomp(CalComponent::get_first_component(icomp, CalComponent::VTIMEZONE_COMPONENT), false);
         tcomp;
         tcomp = CalComponent::get_next_component(icomp, CalComponent::VTIMEZONE_COMPONENT)) {
#ifdef HAVE_LIBECAL_2_0
        eptr<ICalTimezone> zone(i_cal_timezone_new(), "icaltimezone");
        i_cal_timezone_set_component(zone, tcomp);
#else
        eptr<icaltimezone> zone(icaltimezone_new(), "icaltimezone");
        icaltimezone_set_component(zone, tcomp);
#endif

        GErrorCXX gerror;
        const char *tzid;

#ifdef HAVE_LIBECAL_2_0
        tzid = i_cal_timezone_get_tzid(zone);
#else
        tzid = icaltimezone_get_tzid(zone);
#endif
        if (!tzid || !tzid[0]) {
            // cannot add a VTIMEZONE without TZID
            SE_LOG_DEBUG(getDisplayName(), "skipping VTIMEZONE without TZID");
        } else {
            gboolean success =
#ifdef USE_EDS_CLIENT
                e_cal_client_add_timezone_sync(m_calendar, zone, nullptr, gerror)
#else
                e_cal_add_timezone(m_calendar, zone, gerror)
#endif
                ;
            if (!success) {
                throwError(SE_HERE, string("error adding VTIMEZONE ") + tzid,
                           gerror);
            }
        }
    }

    // the component to update/add must be the
    // ICAL_VEVENT/VTODO_COMPONENT of the item,
    // e_cal_create/modify_object() fail otherwise
    CalComponent subcomp(CalComponent::get_first_component(icomp,
                                                           getCompType()),
                         false);
    if (!subcomp) {
        throwError(SE_HERE, "extracting event");
    }

    // Remove LAST-MODIFIED: the Evolution Exchange Connector does not
    // properly update this property if it is already present in the
    // incoming data.
#ifdef HAVE_LIBECAL_2_0
    e_cal_util_component_remove_property_by_kind(subcomp, I_CAL_LASTMODIFIED_PROPERTY, TRUE);
#else
    icalproperty *modprop;
    while ((modprop = icalcomponent_get_first_property(subcomp, ICAL_LASTMODIFIED_PROPERTY)) != nullptr) {
        icalcomponent_remove_property(subcomp, modprop);
        icalproperty_free(modprop);
        modprop = nullptr;
    }
#endif

    if (!update) {
        ItemID id = getItemID(subcomp);
        const char *uid = nullptr;

        // Trying to add a normal event which already exists leads to a
        // gerror->domain == E_CALENDAR_ERROR
        // gerror->code == E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS
        // error. Depending on the Evolution version, the subcomp
        // UID gets removed (>= 2.12) or remains unchanged.
        //
        // Existing detached recurrences are silently updated when
        // trying to add them. This breaks our return code and change
        // tracking.
        //
        // Escape this madness by checking the existence ourselve first
        // based on our list of existing LUIDs. Note that this list is
        // not updated during a sync. This is correct as long as no LUID
        // gets used twice during a sync (examples: add + add, delete + add),
        // which should never happen.
        newluid = id.getLUID();
        if (m_allLUIDs.containsLUID(id)) {
            state = ITEM_NEEDS_MERGE;
        } else {
            // if this is a detached recurrence, then we
            // must use e_cal_modify_object() below if
            // the parent or any other child already exists
            if (!id.m_rid.empty() &&
                m_allLUIDs.containsUID(id.m_uid)) {
                detached = true;
            } else {
                // Creating the parent while children are already in
                // the calendar confuses EDS (at least 2.12): the
                // parent is stored in the .ics with the old UID, but
                // the uid returned to the caller is a different
                // one. Retrieving the item then fails. Avoid this
                // problem by removing the children from the calendar,
                // adding the parent, then updating it with the
                // saved children.
                //
                // TODO: still necessary with e_cal_client API?
                ICalComps_t children;
                if (id.m_rid.empty()) {
                    children = removeEvents(id.m_uid, true);
                }

                // creating new objects works for normal events and detached occurrences alike
                if (
#ifdef USE_EDS_CLIENT
                    e_cal_client_create_object_sync(m_calendar, subcomp,
#ifdef HAVE_LIBECAL_2_0
                                                    E_CAL_OPERATION_FLAG_NONE,
#endif
                                                    (gchar **)&uid, nullptr, gerror)
#else
                    e_cal_create_object(m_calendar, subcomp, (gchar **)&uid, gerror)
#endif
                    ) {
#ifdef USE_EDS_CLIENT
                    PlainGStr owner((gchar *)uid);
#endif
                    // Evolution workaround: don't rely on uid being set if we already had
                    // one. In Evolution 2.12.1 it was set to garbage. The recurrence ID
                    // shouldn't have changed either.
                    ItemID newid(!id.m_uid.empty() ? id.m_uid : uid, id.m_rid);
                    newluid = newid.getLUID();
                    modTime = getItemModTime(newid);
                    m_allLUIDs.insertLUID(newid);
                } else {
                    throwError(SE_HERE, "storing new item", gerror);
                }

                // Recreate any children removed earlier: when we get here,
                // the parent exists and we must update it.
                for (std::shared_ptr< eptr<CalComponent::T> > &icalcomp: children) {
                    if (
#ifdef USE_EDS_CLIENT
                        !e_cal_client_modify_object_sync(m_calendar, *icalcomp,
#ifdef HAVE_LIBECAL_2_0
                                                         E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE,
#else
                                                         CALOBJ_MOD_THIS,
#endif
                                                         nullptr, gerror)
#else
                        !e_cal_modify_object(m_calendar, *icalcomp,
                                             CALOBJ_MOD_THIS,
                                             gerror)
#endif
                        ) {
                        throwError(SE_HERE, string("recreating item ") + newluid, gerror);
                    }
                }
            }
        }
    }

    if (update ||
        (state != ITEM_OKAY && state != ITEM_NEEDS_MERGE) ||
        detached) {
        ItemID id(newluid);
        bool isParent = id.m_rid.empty();

        // ensure that the component has the right UID and
        // RECURRENCE-ID
        if (update) {
            if (!id.m_uid.empty()) {
#ifdef HAVE_LIBECAL_2_0
                i_cal_component_set_uid(subcomp, id.m_uid.c_str());
#else
                icalcomponent_set_uid(subcomp, id.m_uid.c_str());
#endif
            }
            if (!id.m_rid.empty()) {
                // Reconstructing the RECURRENCE-ID is non-trivial,
                // because our luid only contains the date-time, but
                // not the time zone. Only do the work if the event
                // really doesn't have a RECURRENCE-ID.
#ifdef HAVE_LIBECAL_2_0
                ICalTime *rid;
                rid = i_cal_component_get_recurrenceid(subcomp);
                if (!rid || i_cal_time_is_null_time(rid)) {
                    // Preserve the original RECURRENCE-ID, including
                    // timezone, no matter what the update contains
                    // (might have wrong timezone or UTC).
                    eptr<ICalComponent> orig(retrieveItem(id));
                    ICalProperty *orig_rid = i_cal_component_get_first_property(orig, I_CAL_RECURRENCEID_PROPERTY);
                    if (orig_rid) {
                        i_cal_component_take_property(subcomp, i_cal_property_clone(orig_rid));
                    }
                    g_clear_object(&orig_rid);
                }
                g_clear_object(&rid);
#else
                struct icaltimetype rid;
                rid = icalcomponent_get_recurrenceid(subcomp);
                if (icaltime_is_null_time(rid)) {
                    // Preserve the original RECURRENCE-ID, including
                    // timezone, no matter what the update contains
                    // (might have wrong timezone or UTC).
                    eptr<icalcomponent> orig(retrieveItem(id));
                    icalproperty *orig_rid = icalcomponent_get_first_property(orig, ICAL_RECURRENCEID_PROPERTY);
                    if (orig_rid) {
                        icalcomponent_add_property(subcomp, icalproperty_new_clone(orig_rid));
                    }
                }
#endif
            }
        }

        if (isParent) {
            // CALOBJ_MOD_THIS for parent items (UID set, no RECURRENCE-ID)
            // is not supported by all backends: the Exchange Connector
            // fails with it. It might be an incorrect usage of the API.
            // Therefore we have to use CALOBJ_MOD_ALL, but that removes
            // children.
            bool hasChildren = false;
            auto it = m_allLUIDs.find(id.m_uid);
            if (it != m_allLUIDs.end()) {
                for (const string &rid: it->second) {
                    if (!rid.empty()) {
                        hasChildren = true;
                        break;
                    }
                }
            }

            if (hasChildren) {
                // Use CALOBJ_MOD_ALL and temporarily remove
                // the children, then add them again. Otherwise they would
                // get deleted.
                ICalComps_t children = removeEvents(id.m_uid, true);

                // Parent is gone, too, and needs to be recreated.
                const char *uid = nullptr;
                if (
#ifdef USE_EDS_CLIENT
                    !e_cal_client_create_object_sync(m_calendar, subcomp,
#ifdef HAVE_LIBECAL_2_0
                                                     E_CAL_OPERATION_FLAG_NONE,
#endif
                                                     (char **)&uid, nullptr, gerror)
#else
                    !e_cal_create_object(m_calendar, subcomp, (char **)&uid, gerror)
#endif
                    ) {
                    throwError(SE_HERE, string("creating updated item ") + luid, gerror);
                }
#ifdef USE_EDS_CLIENT
                PlainGStr owner((gchar *)uid);
#endif

                // Recreate any children removed earlier: when we get here,
                // the parent exists and we must update it.
                for (std::shared_ptr< eptr<CalComponent::T> > &icalcomp: children) {
                    if (
#ifdef USE_EDS_CLIENT
                        !e_cal_client_modify_object_sync(m_calendar, *icalcomp,
#ifdef HAVE_LIBECAL_2_0
                                                         E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE,
#else
                                                         CALOBJ_MOD_THIS,
#endif
                                                         nullptr, gerror)
#else
                        !e_cal_modify_object(m_calendar, *icalcomp,
                                             CALOBJ_MOD_THIS,
                                             gerror)
#endif
                        ) {
                        throwError(SE_HERE, string("recreating item ") + luid, gerror);
                    }
                }
            } else {
                // no children, updating is simple
                if (
#ifdef USE_EDS_CLIENT
                    !e_cal_client_modify_object_sync(m_calendar, subcomp,
#ifdef HAVE_LIBECAL_2_0
                                                     E_CAL_OBJ_MOD_ALL, E_CAL_OPERATION_FLAG_NONE,
#else
                                                     CALOBJ_MOD_ALL,
#endif
                                                     nullptr, gerror)
#else
                    !e_cal_modify_object(m_calendar, subcomp,
                                         CALOBJ_MOD_ALL,
                                         gerror)
#endif
                    ) {
                    throwError(SE_HERE, string("updating item ") + luid, gerror);
                }
            }
        } else {
            // child event
            if (
#ifdef USE_EDS_CLIENT
                !e_cal_client_modify_object_sync(m_calendar, subcomp,
#ifdef HAVE_LIBECAL_2_0
                                                 E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE,
#else
                                                 CALOBJ_MOD_THIS,
#endif
                                                 nullptr, gerror)
#else
                !e_cal_modify_object(m_calendar, subcomp,
                                     CALOBJ_MOD_THIS,
                                     gerror)
#endif
                ) {
                throwError(SE_HERE, string("updating item ") + luid, gerror);
            }
        }

        ItemID newid = getItemID(subcomp);
        newluid = newid.getLUID();
        modTime = getItemModTime(newid);
    }

    return InsertItemResult(newluid, modTime, state);
}

EvolutionCalendarSource::ICalComps_t EvolutionCalendarSource::removeEvents(const string &uid, bool returnOnlyChildren, bool ignoreNotFound)
{
    ICalComps_t events;

    auto it = m_allLUIDs.find(uid);
    if (it != m_allLUIDs.end()) {
        for (const string &rid: it->second) {
            ItemID id(uid, rid);
            // Always free the component unless we explicitly steal it.
            CalComponent icomp(retrieveItem(id), true);
            if (icomp) {
                if (!id.m_rid.empty() || !returnOnlyChildren) {
                    events.push_back(ICalComps_t::value_type(new eptr<CalComponent::T>(icomp.Steal())));
                }
            }
        }
    }

    // removes all events with that UID, including children
    GErrorCXX gerror;
    if (!uid.empty() && // e_cal_client_remove_object_sync() in EDS 3.8 aborts the process for empty UID, other versions cannot succeed, so skip the call.
#ifdef USE_EDS_CLIENT
        !e_cal_client_remove_object_sync(m_calendar, uid.c_str(), nullptr,
#ifdef HAVE_LIBECAL_2_0
                                         E_CAL_OBJ_MOD_ALL, E_CAL_OPERATION_FLAG_NONE,
#else
                                         CALOBJ_MOD_ALL,
#endif
                                         nullptr, gerror)

#else
        !e_cal_remove_object(m_calendar,
                             uid.c_str(),
                             gerror)
#endif
        ) {
        if (IsCalObjNotFound(gerror)) {
            SE_LOG_DEBUG(getDisplayName(), "%s: request to delete non-existant item ignored",
                         uid.c_str());
            if (!ignoreNotFound) {
                throwError(SE_HERE, STATUS_NOT_FOUND, string("delete item: ") + uid);
            }
        } else {
            throwError(SE_HERE, string("deleting item " ) + uid, gerror);
        }
    }

    return events;
}

void EvolutionCalendarSource::removeItem(const string &luid)
{
    GErrorCXX gerror;
    ItemID id(luid);

    if (id.m_rid.empty()) {
        /*
         * Removing the parent item also removes all children. Evolution
         * does that automatically. Calling e_cal_remove_object_with_mod()
         * without valid rid confuses Evolution, don't do it. As a workaround
         * remove all items with the given uid and if we only wanted to
         * delete the parent, then recreate the children.
         */
        ICalComps_t children = removeEvents(id.m_uid, true, TRANSFER_REF);

        // recreate children
        bool first = true;
        for (std::shared_ptr< eptr<CalComponent::T> > &icalcomp: children) {
            if (first) {
                char *uid;

                if (
#ifdef USE_EDS_CLIENT
                    !e_cal_client_create_object_sync(m_calendar, *icalcomp,
#ifdef HAVE_LIBECAL_2_0
                                                     E_CAL_OPERATION_FLAG_NONE,
#endif
                                                     &uid, nullptr, gerror)
#else
                    !e_cal_create_object(m_calendar, *icalcomp, &uid, gerror)
#endif
                    ) {
                    throwError(SE_HERE, string("recreating first item ") + luid, gerror);
                }
#ifdef USE_EDS_CLIENT
                PlainGStr owner((gchar *)uid);
#endif
                first = false;
            } else {
                if (
#ifdef USE_EDS_CLIENT
                    !e_cal_client_modify_object_sync(m_calendar, *icalcomp,
#ifdef HAVE_LIBECAL_2_0
                                                     E_CAL_OBJ_MOD_THIS, E_CAL_OPERATION_FLAG_NONE,
#else
                                                     CALOBJ_MOD_THIS,
#endif
                                                     nullptr, gerror)
#else
                    !e_cal_modify_object(m_calendar, *icalcomp,
                                         CALOBJ_MOD_THIS,
                                         gerror)
#endif
                    ) {
                    throwError(SE_HERE, string("recreating following item ") + luid, gerror);
                }
            }
        }
    } else {
        // workaround for EDS 2.32 API semantic: succeeds even if
        // detached recurrence doesn't exist and adds EXDATE,
        // therefore we have to check for existence first
        eptr<CalComponent::T> item(retrieveItem(id));
        gboolean success = !item ? false :
#ifdef USE_EDS_CLIENT
            // TODO: is this necessary?
            e_cal_client_remove_object_sync(m_calendar,
                                            id.m_uid.c_str(),
                                            id.m_rid.c_str(),
#ifdef HAVE_LIBECAL_2_0
                                            E_CAL_OBJ_MOD_ONLY_THIS,
					    E_CAL_OPERATION_FLAG_NONE,
#else
                                            CALOBJ_MOD_ONLY_THIS,
#endif
                                            nullptr,
                                            gerror)
#else
            e_cal_remove_object_with_mod(m_calendar,
                                         id.m_uid.c_str(),
                                         id.m_rid.c_str(),
                                         CALOBJ_MOD_THIS,
                                         gerror)
#endif
            ;
        if (!item ||
            (!success && IsCalObjNotFound(gerror))) {
            SE_LOG_DEBUG(getDisplayName(), "%s: request to delete non-existant item",
                         luid.c_str());
            throwError(SE_HERE, STATUS_NOT_FOUND, string("delete item: ") + id.getLUID());
        } else if (!success) {
            throwError(SE_HERE, string("deleting item " ) + luid, gerror);
        }
    }
    m_allLUIDs.eraseLUID(id);

    if (!id.m_rid.empty()) {
        // Removing the child may have modified the parent.
        // We must record the new LAST-MODIFIED string,
        // otherwise it might be reported as modified during
        // the next sync (timing dependent: if the parent
        // was updated before removing the child *and* the
        // update and remove fall into the same second,
        // then the modTime does not change again during the
        // removal).
        try {
            ItemID parent(id.m_uid, "");
            string modTime = getItemModTime(parent);
            string parentLUID = parent.getLUID();
            updateRevision(getTrackingNode(), parentLUID, parentLUID, modTime);
        } catch (...) {
            // There's no guarantee that the parent still exists.
            // Instead of checking that, ignore errors (a bit hacky,
            // but better than breaking the removal).
        }
    }
}

// The caller owns the item.
CalComponent::T *EvolutionCalendarSource::retrieveItem(const ItemID &id)
{
    GErrorCXX gerror;
    CalComponent::T *comp = nullptr;

    if (
#ifdef USE_EDS_CLIENT
        !e_cal_client_get_object_sync(m_calendar,
                                      id.m_uid.c_str(),
                                      !id.m_rid.empty() ? id.m_rid.c_str() : nullptr,
                                      &comp,
                                      nullptr,
                                      gerror)
#else
        !e_cal_get_object(m_calendar,
                          id.m_uid.c_str(),
                          !id.m_rid.empty() ? id.m_rid.c_str() : nullptr,
                          &comp,
                          gerror)
#endif
        ) {
        if (IsCalObjNotFound(gerror)) {
            throwError(SE_HERE, STATUS_NOT_FOUND, string("retrieving item: ") + id.getLUID());
        } else {
            throwError(SE_HERE, string("retrieving item: ") + id.getLUID(), gerror);
        }
    }
    if (!comp) {
        throwError(SE_HERE, string("retrieving item: ") + id.getLUID());
    }
    eptr<CalComponent::T> ptr(comp);

    /*
     * EDS bug: if a parent doesn't exist while a child does, and we ask
     * for the parent, we are sent the (first?) child. Detect this and
     * turn it into a "not found" error.
     */
    if (id.m_rid.empty()) {
#ifdef HAVE_LIBECAL_2_0
        ICalTime *rid = i_cal_component_get_recurrenceid(comp);
        if (!rid || i_cal_time_is_null_time(rid)) {
            g_clear_object(&rid);
        } else {
#else
        struct icaltimetype rid = icalcomponent_get_recurrenceid(comp);
        if (!icaltime_is_null_time(rid)) {
#endif
            throwError(SE_HERE, string("retrieving item: got child instead of parent: ") + id.m_uid);
        }
    }

    return ptr.release();
}

string EvolutionCalendarSource::retrieveItemAsString(const ItemID &id)
{
    eptr<CalComponent::T> comp(retrieveItem(id));
    eptr<char> icalstr;

#ifdef USE_EDS_CLIENT
    icalstr = e_cal_client_get_component_as_string(m_calendar, comp);
#else
    icalstr = e_cal_get_component_as_string(m_calendar, comp);
#endif

    if (!icalstr) {
        // One reason why e_cal_get_component_as_string() can fail is
        // that it uses a TZID which has no corresponding VTIMEZONE
        // definition. Evolution GUI ignores the TZID and interprets
        // the times as local time. Do the same when exporting the
        // event by removing the bogus TZID.
#ifdef HAVE_LIBECAL_2_0
        ICalProperty *prop;
	for (prop = i_cal_component_get_first_property (comp, I_CAL_ANY_PROPERTY);
             prop;
             g_object_unref(prop), prop = i_cal_component_get_next_property (comp, I_CAL_ANY_PROPERTY)) {
            // removes only the *first* TZID - but there shouldn't be more than one
            i_cal_property_remove_parameter_by_kind(prop, I_CAL_TZID_PARAMETER);
        }
#else
        icalproperty *prop = icalcomponent_get_first_property (comp,
                                                               ICAL_ANY_PROPERTY);

        while (prop) {
            // removes only the *first* TZID - but there shouldn't be more than one
            icalproperty_remove_parameter_by_kind(prop, ICAL_TZID_PARAMETER);
            prop = icalcomponent_get_next_property (comp,
                                                    ICAL_ANY_PROPERTY);
        }
#endif

        // now try again
#ifdef USE_EDS_CLIENT
        icalstr = e_cal_client_get_component_as_string(m_calendar, comp);
#else
        icalstr = e_cal_get_component_as_string(m_calendar, comp);
#endif
        if (!icalstr) {
            throwError(SE_HERE, string("could not encode item as iCalendar: ") + id.getLUID());
        } else {
            SE_LOG_DEBUG(getDisplayName(), "had to remove TZIDs because e_cal_get_component_as_string() failed for:\n%s", icalstr.get());
	}
    }

    /*
     * Evolution/libical can only deal with \, as separator.
     * Replace plain \, in outgoing event CATEGORIES with , -
     * based on simple text search/replace and thus will not work
     * in all cases...
     *
     * Inverse operation in insertItem().
     */
    string data = string(icalstr);
    size_t propstart = data.find("\nCATEGORIES");
    bool modified = false;
    while (propstart != data.npos) {
        size_t eol = data.find('\n', propstart + 1);
        size_t comma = data.find(',', propstart);

        while (eol != data.npos &&
               comma != data.npos &&
               comma < eol) {
            if (data[comma-1] == '\\') {
                data.erase(comma - 1, 1);
                comma--;
                modified = true;
            }
            comma = data.find(',', comma + 1);
        }
        propstart = data.find("\nCATEGORIES", propstart + 1);
    }
    if (modified) {
        SE_LOG_DEBUG(getDisplayName(), "after replacing \\, with , in CATEGORIES:\n%s", data.c_str());
    }
    
    return data;
}

std::string EvolutionCalendarSource::getDescription(const string &luid)
{
    try {
        eptr<CalComponent::T> comp(retrieveItem(ItemID(luid)));
        std::string descr;

#ifdef HAVE_LIBECAL_2_0
        const char *summary = i_cal_component_get_summary(comp);
#else
        const char *summary = icalcomponent_get_summary(comp);
#endif
        if (summary && summary[0]) {
            descr += summary;
        }
        
        if (m_type == EVOLUTION_CAL_SOURCE_TYPE_EVENTS) {
#ifdef HAVE_LIBECAL_2_0
            const char *location = i_cal_component_get_location(comp);
#else
            const char *location = icalcomponent_get_location(comp);
#endif
            if (location && location[0]) {
                if (!descr.empty()) {
                    descr += ", ";
                }
                descr += location;
            }
        }

        if (m_type == EVOLUTION_CAL_SOURCE_TYPE_MEMOS &&
            descr.empty()) {
            // fallback to first line of body text
#ifdef HAVE_LIBECAL_2_0
            ICalProperty *desc = i_cal_component_get_first_property(comp, I_CAL_DESCRIPTION_PROPERTY);
#else
            icalproperty *desc = icalcomponent_get_first_property(comp, ICAL_DESCRIPTION_PROPERTY);
#endif
            if (desc) {
#ifdef HAVE_LIBECAL_2_0
                const char *text = i_cal_property_get_description(desc);
#else
                const char *text = icalproperty_get_description(desc);
#endif
                if (text) {
                    const char *eol = strchr(text, '\n');
                    if (eol) {
                        descr.assign(text, eol - text);
                    } else {
                        descr = text;
                    }
                }
#ifdef HAVE_LIBECAL_2_0
                g_object_unref(desc);
#endif
            }
        }

        return descr;
    } catch (...) {
        // Instead of failing we log the error and ask
        // the caller to log the UID. That way transient
        // errors or errors in the logging code don't
        // prevent syncs.
        handleException();
        return "";
    }
}

string EvolutionCalendarSource::ItemID::getLUID() const
{
    return getLUID(m_uid, m_rid);
}

string EvolutionCalendarSource::ItemID::getLUID(const string &uid, const string &rid)
{
    return uid + "-rid" + rid;
}

EvolutionCalendarSource::ItemID::ItemID(const string &luid)
{
    size_t ridoff = luid.rfind("-rid");
    if (ridoff != luid.npos) {
        const_cast<string &>(m_uid) = luid.substr(0, ridoff);
        const_cast<string &>(m_rid) = luid.substr(ridoff + strlen("-rid"));
    } else {
        const_cast<string &>(m_uid) = luid;
    }
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::getItemID(ECalComponent *ecomp)
{
    CalComponent icomp(e_cal_component_get_icalcomponent(ecomp));
    if (!icomp) {
        SE_THROW("internal error in getItemID(): ECalComponent without icalcomp");
    }
    return getItemID(icomp);
}

EvolutionCalendarSource::ItemID EvolutionCalendarSource::getItemID(icalcomponent *icomp)
{
    const char *uid;
    struct icaltimetype rid;

    uid = icalcomponent_get_uid(icomp);
    rid = icalcomponent_get_recurrenceid(icomp);
    return ItemID(uid ? uid : "",
                  icalTime2Str(rid));
}

#ifdef HAVE_LIBECAL_2_0
EvolutionCalendarSource::ItemID EvolutionCalendarSource::getItemID(ICalComponent *icomp)
{
    icalcomponent *native_icomp;

    native_icomp = static_cast<icalcomponent *>(i_cal_object_get_native(I_CAL_OBJECT (icomp)));
    if (!native_icomp) {
        SE_THROW("internal error in getItemID(): ICalComponent without native icalcomp");
    }
    return getItemID(native_icomp);
}
#endif

string EvolutionCalendarSource::getItemModTime(ECalComponent *ecomp)
{
#ifdef HAVE_LIBECAL_2_0
    ICalTime *modTime;
    modTime = e_cal_component_get_last_modified(ecomp);
    eptr<ICalTime, ICalTime, UnrefFree<ICalTime> > modTimePtr(modTime);
#else
    struct icaltimetype *modTime;
    e_cal_component_get_last_modified(ecomp, &modTime);
    eptr<struct icaltimetype, struct icaltimetype, UnrefFree<struct icaltimetype> > modTimePtr(modTime);
#endif
    if (!modTimePtr) {
        return "";
    } else {
#ifdef HAVE_LIBECAL_2_0
        return icalTime2Str(modTimePtr.get());
#else
        return icalTime2Str(*modTimePtr.get());
#endif
    }
}

string EvolutionCalendarSource::getItemModTime(const ItemID &id)
{
    if (!needChanges()) {
        return "";
    }
    eptr<CalComponent::T> icomp(retrieveItem(id));
    return getItemModTime(icomp);
}

string EvolutionCalendarSource::getItemModTime(icalcomponent *icomp)
{
    icalproperty *modprop = icalcomponent_get_first_property(icomp, ICAL_LASTMODIFIED_PROPERTY);
    if (!modprop) {
        return "";
    }
    struct icaltimetype modTime = icalproperty_get_lastmodified(modprop);

    return icalTime2Str(modTime);
}

#ifdef HAVE_LIBECAL_2_0
string EvolutionCalendarSource::getItemModTime(ICalComponent *icomp)
{
    icalcomponent *native_icomp = static_cast<icalcomponent *>(i_cal_object_get_native(I_CAL_OBJECT (icomp)));

    return getItemModTime(native_icomp);
}
#endif

string EvolutionCalendarSource::icalTime2Str(const icaltimetype &tt)
{
    static const struct icaltimetype null = { 0 };
    if (!memcmp(&tt, &null, sizeof(null))) {
        return "";
    } else {
        eptr<char> timestr(ical_strdup(icaltime_as_ical_string(tt)));
        if (!timestr) {
            SE_THROW("cannot convert to time string");
        }
        return timestr.get();
    }
}

#ifdef HAVE_LIBECAL_2_0
string EvolutionCalendarSource::icalTime2Str(const ICalTime *tt)
{
    if (tt || !i_cal_time_is_valid_time (tt) || i_cal_time_is_null_time (tt)) {
        return "";
    } else {
        eptr<char> timestr(i_cal_time_as_ical_string(tt));
        if (!timestr) {
            SE_THROW("cannot convert to time string");
        }
        return timestr.get();
    }
}
#endif

SE_END_CXX

#endif /* ENABLE_ECAL */

#ifdef ENABLE_MODULES
# include "EvolutionCalendarSourceRegister.cpp"
#endif
