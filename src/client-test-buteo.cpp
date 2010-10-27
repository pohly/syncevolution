/*
 * Copyright (C) 2010 Intel Corporation
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

#include "syncevo/util.h"
#include "client-test-buteo.h"
#include <libsyncprofile/SyncResults.h>
#include <libsyncprofile/ProfileEngineDefs.h>
#include <libsyncprofile/Profile.h>
#include <libsyncprofile/SyncProfile.h>
#include <syncmlcommon/SyncMLCommon.h>
#include <QDomDocument>
#include <QtDBus>

using namespace SyncEvo;
using namespace Buteo;

static void execCommand(const std::string &cmd, bool check = true)
{
    bool success = system(cmd.c_str()) == 0;
    if (!success && check) {
        throw runtime_error("failed to excute command: " + cmd);
    }
}

bool ButeoTest::m_inited = false;
QString ButeoTest::m_deviceIds[2];
map<string, string> ButeoTest::m_source2storage;

ButeoTest::ButeoTest(const string &server,
        const string &logbase,
        const SyncEvo::SyncOptions &options) :
    m_server(server), m_logbase(logbase), m_options(options)
{
    init();
}

void ButeoTest::init()
{
    if (!m_inited) {
        m_inited = true;
        // generate device ids
        for(int i = 0; i < sizeof(m_deviceIds)/sizeof(m_deviceIds[0]); i++) {
            QString id;
            UUID uuid;
            QTextStream(&id) << "sc-pim-" << uuid.c_str();
            m_deviceIds[i] = id;
        }
        // insert source -> storage mappings
        m_source2storage.insert(std::make_pair("qt_vcard30", "hcontacts"));
        m_source2storage.insert(std::make_pair("kcal_ical20", "hcalendar"));
        m_source2storage.insert(std::make_pair("kcal_itodo20", "htodo"));
        m_source2storage.insert(std::make_pair("kcal_text", "hnotes"));

        //init qcoreapplication
        static const char *argv[] = { "SyncEvolution" };
        static int argc = 1;
        new QCoreApplication(argc, (char **)argv);
    }
}

void ButeoTest::prepareSources(const int *sources,
        const vector<string> &source2Config) 
{
    for(int i = 0; sources[i] >= 0; i++) {
        string source = source2Config[sources[i]];
        map<string, string>::iterator it = m_source2storage.find(source);
        if (it != m_source2storage.end()) {
            m_configedSources.insert(it->second);
        } else {
            throw runtime_error("unsupported source '" + source + "'");
        }
    }
}

SyncMLStatus ButeoTest::doSync(SyncReport *report) 
{
    SyncMLStatus status = STATUS_OK;

    killAllMsyncd();
    //set sync options
    setupOptions();

    // restore qtcontacts
    if (boost::ends_with(m_server, "_1")) {
        QtContactsSwitcher::restoreStorage("1");
    } else {
        QtContactsSwitcher::restoreStorage("2");
    }
    //start msyncd
    int pid = startMsyncd();

    //kill 'sh' process which is the parent of 'msyncd'
    stringstream cmd;
    cmd << "kill -9 " << pid;
    //run sync
    if (!run()) {
        Execute(cmd.str(), ExecuteFlags(EXECUTE_NO_STDERR | EXECUTE_NO_STDOUT));
        killAllMsyncd();
        return STATUS_FATAL;
    }

    Execute(cmd.str(), ExecuteFlags(EXECUTE_NO_STDERR | EXECUTE_NO_STDOUT));
    killAllMsyncd();

    // save qtcontacts
    if (boost::ends_with(m_server, "_1")) {
        QtContactsSwitcher::backupStorage("1");
    } else {
        QtContactsSwitcher::backupStorage("2");
    }

    //get sync results
    genSyncResults(m_syncResults, report);

    return report->getStatus();
}

void ButeoTest::setupOptions()
{
    // 1. set deviceid, max-message-size options to /etc/sync/meego-sync-conf.xml
    QString meegoSyncmlConf = "/etc/sync/meego-syncml-conf.xml";
    QFile syncmlFile(meegoSyncmlConf);
    if (!syncmlFile.open(QIODevice::ReadOnly)) {
        throw runtime_error("can't open syncml config");
    }
    // don't invoke buteo-syncml API for it doesn't support flushing
    QString syncmlContent(syncmlFile.readAll());
    syncmlFile.close();
    int id = 0;
    if (!boost::ends_with(m_server, "_1")) {
        id = 1; 
    }

    //specify the db path which saves anchors related info, then we can wipe
    //out it if want to slow sync.
    replaceElement(syncmlContent, "dbpath", QString((m_server + ".db").c_str()));

    replaceElement(syncmlContent, "local-device-name", m_deviceIds[id]);

    QString msgSize;
    QTextStream(&msgSize) << m_options.m_maxMsgSize;
    replaceElement(syncmlContent, "max-message-size", msgSize);

    writeToFile(meegoSyncmlConf, syncmlContent);

    // 2. set storage 'Notebook Name' for calendar, todo and notes
    // for contacts, we have to set corresponding tracker db
    string storageDir = getHome() + "/.sync/profiles/storage/"; 
    BOOST_FOREACH(const string &source, m_configedSources) {
        if (boost::iequals(source, "hcalendar") ||
                boost::iequals(source, "htodo") ||
                boost::iequals(source, "hnotes")) {
            string filePath = storageDir + source + ".xml";
            //changeAttrValue(content, "Notebook Name", "value", notebookName, filePath);
            QDomDocument doc(m_server.c_str());
            buildDomFromFile(doc, filePath.c_str());
            QString notebookName;
            QTextStream(&notebookName) << "client_test_" << id;
            Profile profile(doc.documentElement());
            profile.setKey("Notebook Name", notebookName);
            writeToFile(filePath.c_str(), profile.toString());
        } else if (boost::iequals(source, "hcontacts")) {
            // TODO: select correct tracker db
        }
    }
    
    // 3. set wbxml option, sync mode, enabled selected sources and disable other sources 
    QDomDocument doc(m_server.c_str());

    //copy profile
    string profileDir = getHome() + "/.sync/profiles/sync/";
    string profilePath = profileDir + m_server + ".xml";
    size_t pos = m_server.rfind('_');
    if (pos != m_server.npos) {
        string prefix = m_server.substr(0, pos);
        string cmd = "cp ";
        cmd += profileDir;
        cmd += prefix;
        cmd += ".xml ";
        cmd += profilePath;
        execCommand(cmd);
    }

    buildDomFromFile(doc, profilePath.c_str());

    SyncProfile syncProfile(doc.documentElement());
    syncProfile.setName(m_server.c_str());
    QList<Profile *> storages = syncProfile.storageProfilesNonConst();
    QListIterator<Profile *> it(storages);
    while (it.hasNext()) {
        Profile * profile = it.next();
        set<string>::iterator configedIt = m_configedSources.find(profile->name().toStdString());
        if (configedIt != m_configedSources.end()) {
            profile->setKey(KEY_ENABLED, "true");
        } else {
            profile->setKey(KEY_ENABLED, "false");
        }
    }

    // set syncml client
    Profile * syncml = syncProfile.subProfile("syncml", "client");
    if (syncml) {
        // set whether using wbxml
        syncml->setBoolKey(PROF_USE_WBXML, m_options.m_isWBXML);
        // set sync mode
        QString syncMode;
        switch(m_options.m_syncMode) {
        case SYNC_NONE:
            break;
        case SYNC_TWO_WAY:
            syncMode = VALUE_TWO_WAY;
            break;
        case SYNC_ONE_WAY_FROM_CLIENT:
        case SYNC_REFRESH_FROM_CLIENT:
            // work around here since buteo doesn't support refresh mode now
            syncMode = VALUE_TO_REMOTE;
            break;
        case SYNC_ONE_WAY_FROM_SERVER:
        case SYNC_REFRESH_FROM_SERVER:
            syncMode = VALUE_FROM_REMOTE;
            break;
        case SYNC_SLOW: {
            //workaround here since buteo doesn't support explicite slow-sync
            //wipe out anchors so we will do slow sync
            string cmd = "rm -f ";
            cmd += m_server;
            cmd += ".db";
            Execute(cmd, ExecuteFlags(EXECUTE_NO_STDERR | EXECUTE_NO_STDOUT));
            syncMode = VALUE_TWO_WAY;
            break;
        }
        default:
            break;
        }
        syncml->setKey(KEY_SYNC_DIRECTION, syncMode);
    }
    writeToFile(profilePath.c_str(), syncProfile.toString());
}

void ButeoTest::killAllMsyncd()
{
    //firstly killall msyncd
    string cmd = "killall -9 msyncd >/dev/null 2>&1";
    //execCommand(cmd, false);
    Execute(cmd, ExecuteFlags(EXECUTE_NO_STDERR | EXECUTE_NO_STDOUT));
}

int ButeoTest::startMsyncd()
{
    string cmd;
    int pid = fork();
    if (pid == 0) {
        cmd = "msyncd >";
        cmd += m_logbase;
        cmd += ".log 2>&1";
        //children
        if (execlp("sh", "sh", "-c", cmd.c_str(), (char *)0) < 0 ) {
            exit(1);
        }
    } else if (pid < 0) {
        throw runtime_error("can't fork process");
    }
    // wait for msyncd get prepared
    cmd = "sleep 2";
    execCommand(cmd);
    return pid;
}

bool ButeoTest::run()
{
    static const QString msyncdService = "com.meego.msyncd";
    static const QString msyncdObject = "/synchronizer";
    static const QString msyncdInterface = "com.meego.msyncd";

    QDBusConnection conn = QDBusConnection::sessionBus();
    QDBusInterface *interface = new QDBusInterface(msyncdService, msyncdObject, msyncdInterface, conn);
    if (!interface->isValid()) {
        QString error = interface->lastError().message();
        delete interface;
        return false;
    }

    // add watcher for watching unregistering service
    QDBusServiceWatcher *dbusWatcher = new QDBusServiceWatcher(msyncdService, conn, QDBusServiceWatcher::WatchForUnregistration);
    dbusWatcher->connect(dbusWatcher, SIGNAL(serviceUnregistered(QString)),
                           this, SLOT(serviceUnregistered(QString)));

    //connect signals
    interface->connect(interface, SIGNAL(syncStatus(QString, int, QString, int)),
                       this, SLOT(syncStatus(QString, int, QString, int)));
    interface->connect(interface, SIGNAL(resultsAvailable(QString, QString)),
                       this, SLOT(resultsAvailable(QString, QString)));

    // start sync
    QDBusReply<bool> reply = interface->call(QString("startSync"), m_server.c_str());
    if (reply.isValid() && !reply.value()) {
        delete dbusWatcher;
        delete interface;
        return false;
    }

    // wait sync completed
    int result = QCoreApplication::exec();

    delete dbusWatcher;
    delete interface;
    return result == 0;
}

void ButeoTest::genSyncResults(const QString &text, SyncReport *report)
{
    QDomDocument domResults;
    if (domResults.setContent(text, true)) {
        SyncResults syncResults(domResults.documentElement());
        // TODO: set minor code
        switch(syncResults.majorCode()) {
        case SyncResults::SYNC_RESULT_SUCCESS:
            report->setStatus(STATUS_OK);
            break;
        case SyncResults::SYNC_RESULT_FAILED:
            report->setStatus(STATUS_FATAL);
            break;
        case SyncResults::SYNC_RESULT_CANCELLED:
            report->setStatus(STATUS_FATAL);
            break;
        };
        QList<TargetResults> targetResults = syncResults.targetResults();
        QListIterator<TargetResults> it(targetResults);
        while (it.hasNext()) {
            // get item sync info
            TargetResults target = it.next();
            SyncSourceReport targetReport;
            // temporary set this mode due to no this information in report
            targetReport.recordFinalSyncMode(m_options.m_syncMode);
            ItemCounts itemCounts = target.localItems();
            targetReport.setItemStat(SyncSourceReport::ITEM_LOCAL,
                                     SyncSourceReport::ITEM_ADDED,
                                     SyncSourceReport::ITEM_TOTAL,
                                     itemCounts.added);
            targetReport.setItemStat(SyncSourceReport::ITEM_LOCAL,
                                     SyncSourceReport::ITEM_UPDATED,
                                     SyncSourceReport::ITEM_TOTAL,
                                     itemCounts.modified);
            targetReport.setItemStat(SyncSourceReport::ITEM_LOCAL,
                                     SyncSourceReport::ITEM_REMOVED,
                                     SyncSourceReport::ITEM_TOTAL,
                                     itemCounts.deleted);

            // get item info for remote
            itemCounts = target.remoteItems();
            targetReport.setItemStat(SyncSourceReport::ITEM_REMOTE,
                                     SyncSourceReport::ITEM_ADDED,
                                     SyncSourceReport::ITEM_TOTAL,
                                     itemCounts.added);
            targetReport.setItemStat(SyncSourceReport::ITEM_REMOTE,
                                     SyncSourceReport::ITEM_UPDATED,
                                     SyncSourceReport::ITEM_TOTAL,
                                     itemCounts.modified);
            targetReport.setItemStat(SyncSourceReport::ITEM_REMOTE,
                                     SyncSourceReport::ITEM_REMOVED,
                                     SyncSourceReport::ITEM_TOTAL,
                                     itemCounts.deleted);
            // set to sync report
            report->addSyncSourceReport(target.targetName().toStdString(), targetReport);
        }
    } else {
        report->setStatus(STATUS_FATAL);
    }
}

void ButeoTest::syncStatus(QString profile, int status, QString message, int moreDetails)
{
    if (profile == m_server.c_str()) {
        switch(status) {
        case 0: // QUEUED
            break;
        case 1: // STARTED
            break;
        case 2: // PROGRESS
            break;
        case 3: // ERROR
        case 5: // ABORTED
            QCoreApplication::exit(1);
            break;
        case 4: // DONE
            QCoreApplication::exit(0);
            break;
        default:
            ;
        }
    }
}

void ButeoTest::resultsAvailable(QString profile, QString syncResults)
{
    if (profile == m_server.c_str()) {
        m_syncResults = syncResults;
    }
}

void ButeoTest::serviceUnregistered(QString service)
{
    QCoreApplication::exit(1);
}

void ButeoTest::writeToFile(const QString &filePath, const QString &content)
{
    // clear tempoary file firstly
    string cmd = "rm -f ";
    cmd += filePath.toStdString();
    cmd += "_tmp >/dev/null 2>&1";
    execCommand(cmd);

    // open temporary file and serialize dom to the file
    QFile file(filePath+"_tmp");
    if (!file.open(QIODevice::WriteOnly)) {
        QString msg;
        QTextStream(&msg) << "can't open file '" << filePath << "' with 'write' mode";
        throw runtime_error(msg.toStdString());
    }
    if (file.write(content.toUtf8()) == -1) {
        file.close();
        QString msg;
        QTextStream(&msg) << "can't write file '" << filePath << "'";
        throw runtime_error(msg.toStdString());
    }
    file.close();

    // move temp file to destination file
    cmd = "mv " + filePath.toStdString() + "_tmp ";
    cmd += filePath.toStdString();
    cmd += " >/dev/null 2>&1";
    execCommand(cmd);
}

void ButeoTest::replaceElement(QString &xml, const QString &elem, const QString &value)
{
    // TODO: use DOM to parse xml
    // currently this could work
    QString startTag = "<" + elem +">";
    QString endTag = "</" + elem +">";

    int start = xml.indexOf(startTag);
    if ( start == -1) {
        return;
    }
    int end = xml.indexOf(endTag, start);
    int pos = start + startTag.size();

    xml.replace(pos, end - pos, value);
}

void ButeoTest::buildDomFromFile(QDomDocument &doc, const QString &filePath)
{
    // open it 
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QString msg;
        QTextStream(&msg) << "can't open profile file '" << filePath << "'";
        throw runtime_error(msg.toStdString());
    }

    // parse it
    if (!doc.setContent(&file)) {
        file.close();
        QString msg;
        QTextStream(&msg) << "can't parse profile file '" << filePath << "'";
        throw runtime_error(msg.toStdString());
    }
    file.close();
}

static bool isButeo()
{
    static bool checked = false;
    static bool useButeo = false;

    if (!checked) {
        const char *buteo = getenv("CLIENT_TEST_BUTEO");
        if (buteo && 
                (boost::equals(buteo, "1") || boost::iequals(buteo, "t"))) {
            useButeo = true;
        }
        checked = true;
    }

    return useButeo;
}

// 3 databases used by tracker to store contacts
string QtContactsSwitcher::m_databases[] = {"meta.db", "contents.db", "fulltext.db"};

void QtContactsSwitcher::restoreStorage(const string &id)
{
    // if CLIENT_TEST_BUTEO is not enabled, skip it for LocalTests may also use it
    if (!isButeo()) {
        return;
    }

    terminate();

    // copy meta.db_1/2 to meta.db
    string testFile = getDatabasePath() + m_databases[0];

    // for the first time meta.db_1/2 doesn't exist, we
    // copy them from default
    if (access((testFile + "_" + id).c_str(), F_OK) < 0) {
        // if default meta.db doesn't exist generate it
        if (access(testFile.c_str(), F_OK) < 0) {
            start();
            terminate();
        } 
        copyDatabases(id);
    } else {
        // else copy them to default database used by tracker
        copyDatabases(id, false);
    }
    start();
}

void QtContactsSwitcher::backupStorage(const string &id)
{
    // if CLIENT_TEST_BUTEO is not enabled, skip it for LocalTests may also use it
    if (!isButeo()) {
        return;
    }

    terminate();
    // copy meta.db to meta.db_1/2
    copyDatabases(id);
    start();
}

string QtContactsSwitcher::getDatabasePath()
{
    static string m_path = getHome() + "/.cache/tracker/";
    return m_path;
}

void QtContactsSwitcher::copyDatabases(const string &id, bool fromDefault)
{
    for (int i = 0; i < sizeof(m_databases)/sizeof(m_databases[0]); i++) {
        string cmd = "cp -f ";
        string src = getDatabasePath() + m_databases[i];
        string dest = src + "_" + id;
        if (!fromDefault) {
            string tmp = src;
            src = dest;
            dest = tmp;
        }
        cmd += src;
        cmd += " ";
        cmd += dest;
        cmd += " >/dev/null 2>&1";
        execCommand(cmd, false);
    }
}

void QtContactsSwitcher::terminate()
{
    string cmd = "tracker-control -t >/dev/null 2>&1";
    execCommand(cmd);
}

void QtContactsSwitcher::start()
{
    // sleep one second to let tracker daemon get prepared
    string cmd = "tracker-control -s >/dev/null 2>&1; sleep 2";
    execCommand(cmd);
}
