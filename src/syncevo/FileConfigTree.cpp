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

#include <string.h>
#include <ctype.h>

#include <syncevo/FileConfigTree.h>
#include <syncevo/IniConfigNode.h>
#include <syncevo/util.h>

#include <boost/algorithm/string/predicate.hpp>

#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

FileConfigTree::FileConfigTree(const std::string &root,
                               SyncConfig::Layout layout) :
    m_root(root),
    m_layout(layout),
    m_readonly(false)
{
}

void FileConfigTree::flush()
{
    for (const auto &node: m_nodes) {
        node.second->flush();
    }
}

void FileConfigTree::reload()
{
    for (const auto &node: m_nodes) {
        node.second->reload();
    }
}

/**
 * remove config files, backup files of config files (with ~ at
 * the end) and empty directories
 */
static bool rm_filter(const std::string &path, bool isDir)
{
    if (isDir) {
        // skip non-empty directories
        ReadDir dir(path);
        return dir.begin() == dir.end();
    } else {
        // only delete well-known files
        return boost::ends_with(path, "/config.ini") ||
            boost::ends_with(path, "/config.ini~") ||
            boost::ends_with(path, "/config.txt") ||
            boost::ends_with(path, "/config.txt~") ||
            boost::ends_with(path, "/.other.ini") ||
            boost::ends_with(path, "/.other.ini~") ||
            boost::ends_with(path, "/.server.ini") ||
            boost::ends_with(path, "/.server.ini~") ||
            boost::ends_with(path, "/.internal.ini") ||
            boost::ends_with(path, "/.internal.ini~") ||
            path.find("/.synthesis/") != path.npos;
    }
}

void FileConfigTree::remove(const std::string &path)
{
    std::string fullpath = m_root + "/" + path;
    clearNodes(fullpath);
    rm_r(fullpath, rm_filter);
}

void FileConfigTree::reset()
{
    for (auto it = m_nodes.begin();
         it != m_nodes.end();
         ++it) {
        if (it->second.use_count() > 1) {
            // If the use count is larger than 1, then someone besides
            // the cache is referencing the node. We cannot force that
            // other entity to drop the reference, so bail out here.
            SE_THROW(it->second->getName() +
                     ": cannot be removed while in use");
        }
    }
    m_nodes.clear();
}

void FileConfigTree::clearNodes(const std::string &fullpath)
{
    NodeCache_t::iterator it;
    it = m_nodes.begin();
    while (it != m_nodes.end()) {
        const std::string &key = it->first;
        if (boost::starts_with(key, fullpath)){
            /* 'it = m_nodes.erase(it);' doesn't make sense
             * because 'map::erase' returns 'void' in gcc. But other 
             * containers like list, vector could work! :( 
             * Below is STL recommended usage. 
             */
            auto erased = it++;
            if (erased->second.use_count() > 1) {
                // same check as in reset()
                SE_THROW(erased->second->getName() +
                         ": cannot be removed while in use");
            }
            m_nodes.erase(erased);
        } else {
            ++it;
        }
    }
}

std::shared_ptr<ConfigNode> FileConfigTree::open(const std::string &path,
                                                   ConfigTree::PropertyType type,
                                                   const std::string &otherId)
{
    std::string fullpath;
    std::string filename;
    
    fullpath = normalizePath(m_root + "/" + path + "/");
    if (type == other) {
        if (m_layout == SyncConfig::SYNC4J_LAYOUT) {
            fullpath += "/changes";
            if (!otherId.empty()) {
                fullpath += "_";
                fullpath += otherId;
            }
            filename = "config.txt";
        } else {
            filename += ".other";
            if (!otherId.empty()) {
                filename += "_";
                filename += otherId;
            }
            filename += ".ini";
        }
    } else {
        filename = type == server ? ".server.ini" :
            m_layout == SyncConfig::SYNC4J_LAYOUT ? "config.txt" :
            type == hidden ? ".internal.ini" :
            "config.ini";
    }

    std::string fullname = normalizePath(fullpath + "/" + filename);
    auto found = m_nodes.find(fullname);
    if (found != m_nodes.end()) {
        return found->second;
    } else if(type != other && type != server) {
        auto node = std::make_shared<IniFileConfigNode>(fullpath, filename, m_readonly);
        return m_nodes[fullname] = node;
    } else {
        auto node = std::make_shared<IniHashConfigNode>(fullpath, filename, m_readonly);
        return m_nodes[fullname] = node;
    }
}

std::shared_ptr<ConfigNode> FileConfigTree::add(const std::string &path,
                                                  const std::shared_ptr<ConfigNode> &node)
{
    auto found = m_nodes.find(path);
    if (found != m_nodes.end()) {
        return found->second;
    } else {
        m_nodes[path] = node;
        return node;
    }
}

static inline bool isNode(const std::string &dir, const std::string &name) {
    struct stat buf;
    std::string fullpath = dir + "/" + name;
    return !stat(fullpath.c_str(), &buf) && S_ISDIR(buf.st_mode);
}
 
std::list<std::string> FileConfigTree::getChildren(const std::string &path)
{
    std::list<std::string> res;

    std::string fullpath;
    fullpath = normalizePath(m_root + "/" + path);

    // first look at existing files
    if (!access(fullpath.c_str(), F_OK)) {
        ReadDir dir(fullpath);
        for (const std::string &entry: dir) {
            if (isNode(fullpath, entry)) {
                res.push_back(entry);
            }
        }
    }

    // Now also add those which have been created,
    // but not saved yet. The full path must be
    // <path>/<childname>/<filename>.
    fullpath += "/";
    for (const auto &node: m_nodes) {
        std::string currpath = node.first;
        if (currpath.size() > fullpath.size() &&
            currpath.substr(0, fullpath.size()) == fullpath) {
            // path prefix matches, now check whether we have
            // a real sibling, i.e. another full path below
            // the prefix
            size_t start = fullpath.size();
            size_t end = currpath.find('/', start);
            if (currpath.npos != end) {
                // Okay, another path separator found.
                // Now make sure we don't have yet another
                // directory level.
                if (currpath.npos == currpath.find('/', end + 1)) {
                    // Insert it if not there yet.
                    std::string name = currpath.substr(start, end - start);
                    if (res.end() == find(res.begin(), res.end(), name)) {
                        res.push_back(name);
                    }
                }
            }
        }
    }

    return res;
}

SE_END_CXX
