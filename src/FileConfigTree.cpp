/*
 * Copyright (C) 2008 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY, TITLE, NONINFRINGEMENT or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307  USA
 */

#include <string.h>
#include <ctype.h>

#include "FileConfigTree.h"
#include "FileConfigNode.h"
#include "SyncEvolutionUtil.h"

#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>

FileConfigTree::FileConfigTree(const string &root,
                               bool oldLayout) :
    m_root(root),
    m_oldLayout(oldLayout)
{
}

string FileConfigTree::getRootPath() const
{
    return normalizePath(m_root);
}

void FileConfigTree::flush()
{
    for (NodeCache_t::iterator it = m_nodes.begin();
         it != m_nodes.end();
         it++) {
        it->second->flush();
    }
}

void FileConfigTree::reset()
{
    m_nodes.clear();
}

boost::shared_ptr<ConfigNode> FileConfigTree::open(const string &path,
                                                   bool hidden,
                                                   const string &changeId)
{
    string fullpath;
    string filename;
    
    fullpath = normalizePath(m_root + "/" + path + "/");
    if (changeId.size()) {
        if (m_oldLayout) {
            fullpath += "changes_";
            fullpath += changeId;
            filename = "config.txt";
        } else {
            filename += ".changes_";
            filename += changeId;
            filename += ".ini";
        }
    } else {
        filename = m_oldLayout ? "config.txt" :
            hidden ? ".internal.ini" :
            "config.ini";
    }

    string fullname = normalizePath(fullpath + "/" + filename);
    NodeCache_t::iterator found = m_nodes.find(fullname);
    if (found != m_nodes.end()) {
        return found->second;
    } else {
        boost::shared_ptr<ConfigNode> node(new FileConfigNode(fullpath, filename));
        pair<NodeCache_t::iterator, bool> inserted = m_nodes.insert(NodeCache_t::value_type(fullname, node));
        return node;
    }
}

static inline bool isNode(const string &dir, struct dirent *entry) {
    struct stat buf;
    string fullpath = dir + "/" + entry->d_name;
    return (!stat(fullpath.c_str(), &buf) && S_ISDIR(buf.st_mode) &&
        strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."));
}
 
list<string> FileConfigTree::getChildren(const string &path)
{
    list<string> res;

    string fullpath;
    fullpath = normalizePath(m_root + "/" + path);

    // first look at existing files
    DIR *dir = opendir(fullpath.c_str());
    if (dir) {
        struct dirent *entry;
        for (entry = readdir(dir); entry; entry = readdir(dir)) {
            if (isNode(fullpath, entry)) {
                res.push_back(entry->d_name);
            }
        }
        closedir(dir);
    }

    // Now also add those which have been created,
    // but not saved yet. The full path must be
    // <path>/<childname>/<filename>.
    fullpath += "/";
    for (NodeCache_t::iterator it = m_nodes.begin();
         it != m_nodes.end();
         it++) {
        string currpath = it->first;
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
                    string name = currpath.substr(start, end - start);
                    if (res.end() == find(res.begin(), res.end(), name)) {
                        res.push_back(name);
                    }
                }
            }
        }
    }

    return res;
}
