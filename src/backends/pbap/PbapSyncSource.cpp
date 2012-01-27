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

#include "src/gdbusxx/gdbus-cxx-bridge.h"

#include <boost/algorithm/string/predicate.hpp>

#include <syncevo/SyncContext.h>
#include <syncevo/declarations.h>
SE_BEGIN_CXX

PbapSyncSource::PbapSyncSource(const SyncSourceParams &params) :
    TrackingSyncSource(params)
{
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
}

bool PbapSyncSource::isEmpty()
{
    return true;
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
}

void PbapSyncSource::readItem(const string &uid, std::string &item, bool raw)
{
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
