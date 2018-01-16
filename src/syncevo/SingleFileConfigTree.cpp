/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#include <config.h>
#include "test.h"
#include <syncevo/SingleFileConfigTree.h>
#include <syncevo/StringDataBlob.h>
#include <syncevo/FileDataBlob.h>
#include <syncevo/IniConfigNode.h>
#include <syncevo/util.h>

#include <boost/algorithm/string.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

SingleFileConfigTree::SingleFileConfigTree(const std::shared_ptr<DataBlob> &data) :
    m_data(data)
{
    readFile();
}

SingleFileConfigTree::SingleFileConfigTree(const std::string &fullpath) :
    m_data(new FileDataBlob(fullpath, true))
{
    readFile();
}

std::shared_ptr<ConfigNode> SingleFileConfigTree::open(const std::string &filename)
{
    std::string normalized = normalizePath(std::string("/") + filename);
    std::shared_ptr<ConfigNode> &entry = m_nodes[normalized];
    if (entry) {
        return entry;
    }

    std::string name = m_data->getName() + " - " + normalized;
    std::shared_ptr<DataBlob> data; 

    for (const auto &file: m_content) {
        if (file.first == normalized) {
            data.reset(new StringDataBlob(name, file.second, true));
            break;
        }
    }
    if (!data) {
        /*
         * creating new files not supported, would need support for detecting
         * StringDataBlob::write()
         */
        data.reset(new StringDataBlob(name, std::shared_ptr<std::string>(), true));
    }
    entry.reset(new IniFileConfigNode(data));
    return entry;
}

void SingleFileConfigTree::flush()
{
    // not implemented, cannot write anyway
}

void SingleFileConfigTree::reload()
{
    SE_THROW("SingleFileConfigTree::reload() not implemented");
}

void SingleFileConfigTree::remove(const std::string &path)
{
    SE_THROW("internal error: SingleFileConfigTree::remove() called");
}

void SingleFileConfigTree::reset()
{
    m_nodes.clear();
    readFile();
}

std::shared_ptr<ConfigNode> SingleFileConfigTree::open(const std::string &path,
                                                         PropertyType type,
                                                         const std::string &otherId)
{
    std::string fullpath = path;
    if (!fullpath.empty()) {
        fullpath += "/";
    }
    switch (type) {
    case visible:
        fullpath += "config.ini";
        break;
    case hidden:
        fullpath += ".internal.ini";
        break;
    case other:
        fullpath += ".other.ini";
        break;
    case server:
        fullpath += ".server.ini";
        break;
    }
    
    return open(fullpath);
}

std::shared_ptr<ConfigNode> SingleFileConfigTree::add(const std::string &path,
                                                        const std::shared_ptr<ConfigNode> &bode)
{
    SE_THROW("SingleFileConfigTree::add() not supported");
}


static void checkChild(const std::string &normalized,
                       const std::string &node,
                       std::set<std::string> &subdirs)
{
    if (boost::starts_with(node, normalized)) {
        std::string remainder = node.substr(normalized.size());
        size_t offset = remainder.find('/');
        if (offset != remainder.npos) {
            // only directories underneath path matter
            subdirs.insert(remainder.substr(0, offset));
        }
    }
}

std::list<std::string> SingleFileConfigTree::getChildren(const std::string &path)
{
    std::set<std::string> subdirs;
    std::string normalized = normalizePath(std::string("/") + path);
    if (normalized != "/") {
        normalized += "/";
    }

    // must check both actual files as well as unsaved nodes
    for (const auto &file: m_content) {
        checkChild(normalized, file.first, subdirs);
    }
    for (const auto &file: m_nodes) {
        checkChild(normalized, file.first, subdirs);
    }

    std::list<std::string> result;
    for (const std::string &dir: subdirs) {
        result.push_back(dir);
    }
    return result;
}

void SingleFileConfigTree::readFile()
{
    std::shared_ptr<std::istream> in(m_data->read());
    std::shared_ptr<std::string> content;
    std::string line;

    m_content.clear();
    while (getline(*in, line)) {
        if (boost::starts_with(line, "=== ") &&
            boost::ends_with(line, " ===")) {
            std::string name = line.substr(4, line.size() - 8);
            name = normalizePath(std::string("/") + name);
            content.reset(new std::string);
            m_content[name] = content;
        } else if (content) {
            (*content) += line;
            (*content) += "\n";
        }
    }
}

#ifdef ENABLE_UNIT_TESTS

class SingleIniTest : public CppUnit::TestFixture {
    CPPUNIT_TEST_SUITE(SingleIniTest);
    CPPUNIT_TEST(simple);
    CPPUNIT_TEST_SUITE_END();

    void simple() {
        auto data = std::make_shared<std::string>();
        data->assign("# comment\n"
                     "# foo\n"
                     "=== foo/config.ini ===\n"
                     "foo = bar\n"
                     "foo2 = bar2\n"
                     "=== foo/.config.ini ===\n"
                     "foo_internal = bar_internal\n"
                     "foo2_internal = bar2_internal\n"
                     "=== /bar/.internal.ini ===\n"
                     "bar = foo\n"
                     "=== sources/addressbook/config.ini ===\n"
                     "=== sources/calendar/config.ini ===\n"
                     "evolutionsource = Personal\n");
        auto blob = std::make_shared<StringDataBlob>("test", data, true);
        SingleFileConfigTree tree(blob);
        std::shared_ptr<ConfigNode> node;
        node = tree.open("foo/config.ini");
        CPPUNIT_ASSERT(node);
        CPPUNIT_ASSERT(node->exists());
        CPPUNIT_ASSERT_EQUAL(std::string("test - /foo/config.ini"), node->getName());
        CPPUNIT_ASSERT(node->readProperty("foo").wasSet());
        CPPUNIT_ASSERT_EQUAL(std::string("bar"), node->readProperty("foo").get());
        CPPUNIT_ASSERT_EQUAL(std::string("bar2"), node->readProperty("foo2").get());
        CPPUNIT_ASSERT(node->readProperty("foo2").wasSet());
        CPPUNIT_ASSERT_EQUAL(std::string(""), node->readProperty("no_such_bar").get());
        CPPUNIT_ASSERT(!node->readProperty("no_such_bar").wasSet());
        node = tree.open("/foo/config.ini");
        CPPUNIT_ASSERT(node);
        CPPUNIT_ASSERT(node->exists());
        node = tree.open("foo//.config.ini");
        CPPUNIT_ASSERT(node);
        CPPUNIT_ASSERT(node->exists());
        CPPUNIT_ASSERT_EQUAL(std::string("bar_internal"), node->readProperty("foo_internal").get());
        CPPUNIT_ASSERT_EQUAL(std::string("bar2_internal"), node->readProperty("foo2_internal").get());
        node = tree.open("bar///./.internal.ini");
        CPPUNIT_ASSERT(node);
        CPPUNIT_ASSERT(node->exists());
        CPPUNIT_ASSERT_EQUAL(std::string("foo"), node->readProperty("bar").get());
        node = tree.open("sources/addressbook/config.ini");
        CPPUNIT_ASSERT(node);
        CPPUNIT_ASSERT(node->exists());
        node = tree.open("sources/calendar/config.ini");
        CPPUNIT_ASSERT(node);
        CPPUNIT_ASSERT(node->exists());
        CPPUNIT_ASSERT_EQUAL(std::string("Personal"), node->readProperty("evolutionsource").get());

        node = tree.open("no-such-source/config.ini");
        CPPUNIT_ASSERT(node);
        CPPUNIT_ASSERT(!node->exists());

        std::list<std::string> dirs = tree.getChildren("");
        CPPUNIT_ASSERT_EQUAL(std::string("bar|foo|no-such-source|sources"), boost::join(dirs, "|"));
        dirs = tree.getChildren("sources/");
        CPPUNIT_ASSERT_EQUAL(std::string("addressbook|calendar"), boost::join(dirs, "|"));
    }
};

SYNCEVOLUTION_TEST_SUITE_REGISTRATION(SingleIniTest);

#endif // ENABLE_UNIT_TESTS


SE_END_CXX
