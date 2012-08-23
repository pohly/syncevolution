/*
 * Copyright (C) 2007-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
 * Copyright (C) 2012 BMW Car IT GmbH. All rights reserved.
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

#ifdef ENABLE_PBAP

#include "PbapSyncSource.h"

// SyncEvolution includes a copy of Boost header files.
// They are safe to use without creating additional
// build dependencies. boost::filesystem requires a
// library and therefore is avoided here. Some
// utility functions from SyncEvolution are used
// instead, plus standard C/Posix functions.
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/tokenizer.hpp>

#include <errno.h>
#include <unistd.h>

#include <syncevo/util.h>

#include "gdbus-cxx-bridge.h"

#include <boost/algorithm/string/predicate.hpp>

#include <syncevo/SyncContext.h>
#include <syncevo/declarations.h>
SE_BEGIN_CXX

#define OBC_SERVICE "org.openobex.client"
#define OBC_CLIENT_INTERFACE "org.openobex.Client"
#define OBC_PBAP_INTERFACE "org.openobex.PhonebookAccess"

class PbapSession {
public:
    PbapSession(void);

    void initSession(const std::string &address);

    typedef std::map<std::string, std::string> Content;
    void pullAll(Content &dst);

    void shutdown(void);

private:
    GDBusCXX::DBusRemoteObject m_client;
    std::auto_ptr<GDBusCXX::DBusRemoteObject> m_session;
};

PbapSession::PbapSession(void) :
    m_client(GDBusCXX::dbus_get_bus_connection("SESSION", NULL, true, NULL),
             "/", OBC_CLIENT_INTERFACE, OBC_SERVICE, true)
{
}

void PbapSession::initSession(const std::string &address)
{
    if (m_session.get()) {
        return;
    }

    typedef std::map<std::string, boost::variant<std::string> > Params;

    GDBusCXX::DBusClientCall1<GDBusCXX::DBusObject_t>
        createSession(m_client, "CreateSession");

    Params params;
    params["Destination"] = std::string(address);
    params["Target"] = std::string("PBAP");

    std::string session = createSession(params);
    if (session.empty()) {
        SE_THROW("PBAP: got empty session from CreateSession()");
    }

    m_session.reset(new GDBusCXX::DBusRemoteObject(m_client.getConnection(),
                                                   session,
                                                   OBC_PBAP_INTERFACE,
                                                   OBC_SERVICE,
                                                   true));

    SE_LOG_DEBUG(NULL, NULL, "PBAP session created: %s", m_session->getPath());

    GDBusCXX::DBusClientCall0 select(*m_session, "Select");
    select(std::string("int"), std::string("PB"));

    SE_LOG_DEBUG(NULL, NULL, "PBAP session initialized");
}

void vcardParse(const std::string &content, std::size_t begin, std::size_t end, std::map<std::string, std::string> &dst)
{
    static const boost::char_separator<char> lineSep("\n\r");

    typedef boost::tokenizer<boost::char_separator<char> > Tokenizer;
    Tokenizer tok(content.begin() + begin, content.begin() + end, lineSep);

    for(Tokenizer::iterator it = tok.begin(); it != tok.end(); it ++) {
        const std::string &line = *it;
        size_t i = line.find(':');
        if(i != std::string::npos) {
            std::size_t j = line.find(';');
            j = (j == std::string::npos)? i : std::min(i, j);
            std::string key = line.substr(0, j);
            std::string value = line.substr(i + 1);
            dst[key] = value;
        }
    }
}

void PbapSession::pullAll(Content &dst)
{
    GDBusCXX::DBusClientCall1<std::string> pullall(*m_session, "PullAll");
    std::string content = pullall();

    // empty content not treated as an error

    typedef std::map<std::string, int> CounterMap;
    CounterMap counterMap;

    std::size_t pos = 0;
    int count = 0;
    while(pos < content.size()) {
        static const std::string beginStr("BEGIN:VCARD");
        static const std::string endStr("END:VCARD");

        pos = content.find(beginStr, pos);
        if(pos == std::string::npos) {
            break;
        }

        std::size_t endPos = content.find(endStr, pos + beginStr.size());
        if(endPos == std::string::npos) {
            break;
        }

        endPos += endStr.size();

        typedef std::map<std::string, std::string> VcardMap;
        VcardMap vcard;
        vcardParse(content, pos, endPos, vcard);

        VcardMap::const_iterator it = vcard.find("N");
        if(it != vcard.end() && !it->second.empty()) {
            const std::string &fn = it->second;

            const std::pair<CounterMap::iterator, bool> &r =
                counterMap.insert(CounterMap::value_type(fn, 0));
            if(!r.second) {
                r.first->second ++;
            }

            char suffix[8];
            sprintf(suffix, "%07d", r.first->second);

            std::string id = StringPrintf("%d", count); // fn + std::string(suffix);
            dst[id] = content.substr(pos, endPos);
        }

        pos = endPos;
        count++;
    }

    SE_LOG_DEBUG(NULL, NULL, "PBAP content pulled: %d entries", (int) dst.size());
}

void PbapSession::shutdown(void)
{
    GDBusCXX::DBusClientCall0 removeSession(m_client, "RemoveSession");

    // always clear pointer, even if method call fails
    GDBusCXX::DBusObject_t path(m_session->getPath());
    m_session.reset();
    SE_LOG_DEBUG(NULL, NULL, "removed session: %s", path.c_str());

    removeSession(path);

    SE_LOG_DEBUG(NULL, NULL, "PBAP session closed");
}

PbapSyncSource::PbapSyncSource(const SyncSourceParams &params) :
    TrackingSyncSource(params)
{
    m_session.reset(new PbapSession());
}

std::string PbapSyncSource::getMimeType() const
{
    return "text/x-vcard";
}

std::string PbapSyncSource::getMimeVersion() const
{
    return "2.1";
}

void PbapSyncSource::open()
{
    const string &database = getDatabaseID();
    const string prefix("obex-bt://");

    if (!boost::starts_with(database, prefix)) {
        throwError("database should specifiy device address (obex-bt://<bt-addr>)");
    }

    std::string address = database.substr(prefix.size());

    m_session->initSession(address);
    m_session->pullAll(m_content);
    m_session->shutdown();
}

bool PbapSyncSource::isEmpty()
{
    return m_content.empty();
}

void PbapSyncSource::close()
{
}

PbapSyncSource::Databases PbapSyncSource::getDatabases()
{
    Databases result;

    result.push_back(Database("select database via bluetooth address",
                              "[obex-bt://]<bt-addr>"));
    return result;
}

void PbapSyncSource::listAllItems(RevisionMap_t &revisions)
{
    typedef std::pair<std::string, std::string> Entry;
    BOOST_FOREACH(const Entry &entry, m_content) {
        revisions[entry.first] = "0";
    }
}

void PbapSyncSource::readItem(const string &uid, std::string &item, bool raw)
{
    Content::iterator it = m_content.find(uid);
    if(it != m_content.end()) {
        item = it->second;
    }
}

TrackingSyncSource::InsertItemResult PbapSyncSource::insertItem(const string &uid, const std::string &item, bool raw)
{
    throw std::runtime_error("Operation not supported");
}

void PbapSyncSource::removeItem(const string &uid)
{
    throw std::runtime_error("Operation not supported");
}

SE_END_CXX

#endif /* ENABLE_PBAP */

#ifdef ENABLE_MODULES
# include "PbapSyncSourceRegister.cpp"
#endif
