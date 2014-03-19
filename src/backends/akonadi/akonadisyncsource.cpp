/*
    Copyright (c) 2009 Sascha Peilicke <sasch.pe@gmx.de>

    This application is free software; you can redistribute it and/or modify it
    under the terms of the GNU Library General Public License as published by
    the Free Software Foundation; either version 2 of the License, or (at your
    option) any later version.

    This application is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Library General Public
    License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this application; see the file COPYING.LIB.  If not, write to the
    Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
    02110-1301, USA.
*/

#include "akonadisyncsource.h"

#ifdef ENABLE_AKONADI

#include <Akonadi/ItemCreateJob>
#include <Akonadi/ItemDeleteJob>
#include <Akonadi/ItemFetchJob>
#include <Akonadi/ItemFetchScope>
#include <Akonadi/ItemModifyJob>

#include <Akonadi/CollectionFetchJob>
#include <Akonadi/CollectionFetchScope>

#include <Akonadi/CollectionStatistics>
#include <Akonadi/CollectionStatisticsJob>

#include <Akonadi/ServerManager>
#include <Akonadi/Control>
#include <kurl.h>

#include <syncevo/util.h>

#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <boost/bind.hpp>

#include <QtCore/QDebug>

SE_BEGIN_CXX
using namespace Akonadi;

/**
 * We take over ownership of jobs by storing them in smart pointers
 * (RAII).  This is how SyncEvolution does things and more predictable
 * than assuming that a future exec() call will auto-delete them as
 * part of its event processing.
 *
 * To avoid double frees, we need to disable auto-deletion.
 * This method does that. Use like this:
 * std::auto_ptr<CollectionStatisticsJob> statisticsJob(DisableAutoDelete(new CollectionStatisticsJob(m_collection)));
 */
template<class J> J *DisableAutoDelete(J *job) { job->setAutoDelete(false); return job; }

AkonadiSyncSource::AkonadiSyncSource(const char *submime,
                                     const SyncSourceParams &params)
    : TrackingSyncSource(params)
{
    m_mimeTypes = QString(submime).split(",", QString::SkipEmptyParts);
}

AkonadiSyncSource::~AkonadiSyncSource()
{
}

bool AkonadiSyncSource::isEmpty()
{
    if (!GRunIsMain()) {
        bool result;
        GRunInMain(boost::lambda::var(result) = boost::lambda::bind(&AkonadiSyncSource::isEmpty, this));
        return result;
    }

    //To Check if the respective collection is Empty, without actually loading the collections
    std::auto_ptr<CollectionStatisticsJob> statisticsJob(DisableAutoDelete(new CollectionStatisticsJob(m_collection)));
    if (!statisticsJob->exec()) {
        throwError("Error fetching the collection stats");
    }
    return statisticsJob->statistics().count() == 0;
}

void AkonadiSyncSource::start()
{
    if (!GRunIsMain()) {
        GRunInMain(boost::bind(&AkonadiSyncSource::start, this));
        return;
    }

    // Check for Akonadi server.
    if (!Akonadi::ServerManager::isRunning()) {
        // Don't try to start it. A normal KDE user should have it already
        // running. Users of other desktop systems probably don't want it
        // to run, if they have it installed at all.
        //
        // Starting it here also produces output that we don't want mixed
        // into normal SyncEvolution command line output.
#if 0
        SE_LOG_DEBUG(NULL, "Akonadi Server isn't running, and hence starting it.");
        if (!Akonadi::Control::start()) {
            SE_THROW("Couldn't Start Akonadi Server: hence the akonadi backend of syncevolution wont work ..");
        }
#else
        SE_THROW("Akonadi is not running. It can be started with 'akonadictl start'.");
#endif
    }
}

SyncSource::Databases AkonadiSyncSource::getDatabases()
{
    if (!GRunIsMain()) {
        Databases result;
        GRunInMain(boost::lambda::var(result) = boost::lambda::bind(&AkonadiSyncSource::getDatabases, this));
        return result;
    }

    start();

    Databases res;
    // Insert databases which match the "type" of the source, including a user-visible
    // description and a database IDs. Exactly one of the databases  should be marked
    // as the default one used by the source.
    // res.push_back("Contacts", "some-KDE-specific-ID", isDefault);

    std::auto_ptr<CollectionFetchJob> fetchJob(DisableAutoDelete(new CollectionFetchJob(Collection::root(),
                                                                                        CollectionFetchJob::Recursive)));

    fetchJob->fetchScope().setContentMimeTypes(m_mimeTypes);

    if (!fetchJob->exec()) {
        throwError("cannot list collections");
    }

    // Currently, the first collection of the right type is the default
    // This decision should go to the GUI: which deals with sync profiles.

    bool isFirst = true;
    Collection::List collections = fetchJob->collections();
    foreach (const Collection &collection, collections) {
        res.push_back(Database(collection.name().toUtf8().constData(),
                               collection.url().url().toUtf8().constData(),
                               isFirst));
        isFirst = false;
    }
    return res;
}

void AkonadiSyncSource::open()
{
    if (!GRunIsMain()) {
        GRunInMain(boost::bind(&AkonadiSyncSource::open, this));
        return;
    }

    start();

    // the "evolutionsource" property, empty for default,
    // otherwise the collection URL or a name
    string id = getDatabaseID();

    // hack for testing: use first resp. second database
    if (boost::starts_with(id, "Test_")) {
        Databases databases = getDatabases();
        ssize_t index = -1;
        if (boost::ends_with(id, "_1")) {
            index = 0;
        } else if (boost::ends_with(id, "_2")) {
            index = 1;
        }
        if (index >= 0) {
            if (databases.size() <= (size_t)index) {
                SE_THROW("need two Akonadi resources for testing");
            }
            id = databases[index].m_uri;
            SE_LOG_DEBUG(NULL, "testing Akonadi with %s", id.c_str());
        }
    }

    if (!boost::starts_with(id, "akonadi:")) {
        // TODO: support selection by name and empty ID for default
        SE_THROW("database must be selected via database = akonadi:?collection=<number>");
    }

    m_collection = Collection::fromUrl(KUrl(id.c_str()));

    // Verify that the collection exists and ensure that
    // m_collection.contentMimeTypes() returns valid information. The
    // collection constructed so far only contains the collection ID.
    std::auto_ptr<CollectionFetchJob> fetchJob(DisableAutoDelete(new CollectionFetchJob(m_collection,
                                                                                        CollectionFetchJob::Base)));
    if (!fetchJob->exec()) {
        throwError(StringPrintf("cannot fetch collection %s", id.c_str()));
    }
    Collection::List collections = fetchJob->collections();
    if (collections.isEmpty()) {
        throwError(StringPrintf("collection %s not found", id.c_str()));
    }
    m_collection = collections.front();

    m_contentMimeType = "";
    QStringList collectionMimeTypes = m_collection.contentMimeTypes();
    foreach (const QString &mimeType, m_mimeTypes) {
        if (collectionMimeTypes.contains(mimeType)) {
            m_contentMimeType = mimeType;
            break;
        }
    }
    if (m_contentMimeType.isEmpty()) {
        throwError(StringPrintf("Resource %s cannot store items of type(s) %s. It can only store %s.",
                                id.c_str(),
                                m_mimeTypes.join(",").toUtf8().constData(),
                                collectionMimeTypes.join(",").toUtf8().constData()));
    }
}

void AkonadiSyncSource::listAllItems(SyncSourceRevisions::RevisionMap_t &revisions)
{
    if (!GRunIsMain()) {
        GRunInMain(boost::bind(&AkonadiSyncSource::listAllItems, this, boost::ref(revisions)));
        return;
    }

    // copy all local IDs and the corresponding revision
    std::auto_ptr<ItemFetchJob> fetchJob(DisableAutoDelete(new ItemFetchJob(m_collection)));
    if (!fetchJob->exec()) {
        throwError("listing items");
    }
    BOOST_FOREACH (const Item &item, fetchJob->items()) {
        // Filter out items which don't have the right type (for example, VTODO when
        // syncing events)
        if (m_mimeTypes.contains(item.mimeType())) {
            revisions[QByteArray::number(item.id()).constData()] =
                      QByteArray::number(item.revision()).constData();
        }
    }
}

void AkonadiSyncSource::close()
{
    // TODO: close collection!?
}

TrackingSyncSource::InsertItemResult AkonadiSyncSource::insertItem(const std::string &luid, const std::string &data, bool raw)
{
    if (!GRunIsMain()) {
        InsertItemResult result;
        GRunInMain(boost::lambda::var(result) = boost::lambda::bind(&AkonadiSyncSource::insertItem, this, boost::cref(luid), boost::cref(data), raw));
        return result;
    }

    Item item;

    if (luid.empty()) {
        item.setMimeType(m_mimeTypes.front());
        item.setPayloadFromData(QByteArray(data.c_str()));
        std::auto_ptr<ItemCreateJob> createJob(DisableAutoDelete(new ItemCreateJob(item, m_collection)));
        if (!createJob->exec()) {
            throwError(string("storing new item ") + luid);
            return InsertItemResult("", "", ITEM_OKAY);
        }
        item = createJob->item();
    } else {
        Entity::Id syncItemId = QByteArray(luid.c_str()).toLongLong();
        std::auto_ptr<ItemFetchJob> fetchJob(DisableAutoDelete(new ItemFetchJob(Item(syncItemId))));
        if (!fetchJob->exec()) {
            throwError(string("checking item ") + luid);
        }
        item = fetchJob->items().first();
        item.setPayloadFromData(QByteArray(data.c_str()));
        std::auto_ptr<ItemModifyJob> modifyJob(DisableAutoDelete(new ItemModifyJob(item)));
        // TODO: SyncEvolution must pass the known revision that
        // we are updating.
        // TODO: check that the item has not been updated in the meantime
        if (!modifyJob->exec()) {
            throwError(string("updating item ") + luid);
            return InsertItemResult("", "", ITEM_OKAY);
        }
        item = modifyJob->item();
    }

    // Read-only datastores may not have actually added something here!
    // The Jobs themselves throw errors, and hence the return statements
    // above will take care of this
    return InsertItemResult(QByteArray::number(item.id()).constData(),
                            QByteArray::number(item.revision()).constData(),
                            ITEM_OKAY);
}

void AkonadiSyncSource::removeItem(const string &luid)
{
    if (!GRunIsMain()) {
        GRunInMain(boost::bind(&AkonadiSyncSource::removeItem, this, boost::cref(luid)));
        return;
    }

    Entity::Id syncItemId = QByteArray(luid.c_str()).toLongLong();

    // Delete the item from our collection
    // TODO: check that the revision is right (need revision from SyncEvolution)
    std::auto_ptr<ItemDeleteJob> deleteJob(DisableAutoDelete(new ItemDeleteJob(Item(syncItemId))));
    if (!deleteJob->exec()) {
        throwError(string("deleting item " ) + luid);
    }
}

void AkonadiSyncSource::readItem(const std::string &luid, std::string &data, bool raw)
{
    if (!GRunIsMain()) {
        GRunInMain(boost::bind(&AkonadiSyncSource::readItem, this, boost::cref(luid), boost::ref(data), raw));
        return;
    }

    Entity::Id syncItemId = QByteArray(luid.c_str()).toLongLong();

    std::auto_ptr<ItemFetchJob> fetchJob(DisableAutoDelete(new ItemFetchJob(Item(syncItemId))));
    fetchJob->fetchScope().fetchFullPayload();
    if (fetchJob->exec()) {
        if (fetchJob->items().empty()) {
            throwError(STATUS_NOT_FOUND, string("extracting item ") + luid);
        }
        QByteArray payload = fetchJob->items().first().payloadData();
        data.assign(payload.constData(),
                    payload.size());
    } else {
        throwError(string("extracting item " ) + luid);
    }
}

QString AkonadiMemoSource::toKJots(QString data){
  // KJots stores it's resource in the format
  //Subject: Hello World
  //Content-Type: text/plain <------- always plain text for the akonadi resource
  //Date: Wed, 30 Mar 2011 01:02:48 +0530 <----date created
  //MIME-Version: 1.0 <----- always the same
  // <---- This line break seperates the content from the information
  //<Content>

  QString subject = "Subject: ";
  QString contentType = "Content-Type: text/plain";
  QString dateTime = QDateTime::currentDateTime().toString(Qt::ISODate);
  QString mimeVersion = "MIME-Version: 1.0";
  QString content;
  
  QStringList lines = data.split('\n');
  subject += lines.first();
  content = data.remove(0,data.indexOf('\n')+1);
  
  QString result = subject + '\n' +
                   contentType + '\n' +
                   dateTime + '\n'+
                   mimeVersion + "\n\n"+
                   content;
  return result;
}

QString AkonadiMemoSource::toSynthesis(QString data){
    //Synthesis expects Plain Text in the form Subject + "\n" + Content
    QString subject;
    QString content;
  
    subject = data.split('\n').first();
    subject.remove("Subject: ");
    
    content = data.remove(0,data.indexOf("\n\n")+2);
    return subject+'\n'+content;
}

void AkonadiMemoSource::readItem(const std::string &luid, std::string &data, bool raw)
{
    AkonadiSyncSource::readItem(luid, data, raw);
    data = toSynthesis(QString::fromStdString(data)).toStdString();
}

TrackingSyncSource::InsertItemResult AkonadiMemoSource::insertItem(const std::string &luid, const std::string &data, bool raw)
{
    std::string formattedData = toKJots(QString::fromStdString(data)).toStdString();
    return AkonadiSyncSource::insertItem(luid, formattedData , raw);
}

SE_END_CXX
#endif // ENABLE_AKONADI

#ifdef ENABLE_MODULES
# include "AkonadiSyncSourceRegister.cpp"
#endif
