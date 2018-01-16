/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
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

#include <syncevo/IniConfigNode.h>
#include <syncevo/FileDataBlob.h>
#include <syncevo/SyncConfig.h>
#include <syncevo/util.h>

#include <boost/scoped_array.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

IniBaseConfigNode::IniBaseConfigNode(const std::shared_ptr<DataBlob> &data) :
    m_data(data)
{
}

void IniBaseConfigNode::flush()
{
    if (!m_modified) {
        return;
    }

    if (m_data->isReadonly()) {
        throw std::runtime_error(m_data->getName() + ": internal error: flushing read-only config node not allowed");
    }

    // Our m_modified check is not perfect: sometimes changes are made
    // that once complete, lead to the exact same file content. Catch
    // that with a brute-force memory compare and avoid rewriting the
    // file unless something changed.
    std::stringstream temp;
    toFile(temp);
    std::string newcontent = temp.str();
    std::shared_ptr<std::istream> oldfile = m_data->read();
    std::string oldcontent;
    if (!ReadFile(*oldfile, oldcontent) ||
        oldcontent != newcontent) {
        std::shared_ptr<std::ostream> newfile = m_data->write();
        *newfile << newcontent;
    }

    m_modified = false;
}

IniFileConfigNode::IniFileConfigNode(const std::shared_ptr<DataBlob> &data) :
    IniBaseConfigNode(data)
{
    read();
}

IniFileConfigNode::IniFileConfigNode(const std::string &path, const std::string &fileName, bool readonly) :
    IniBaseConfigNode(std::make_shared<FileDataBlob>(path, fileName, readonly))
{
    read();
}



void IniFileConfigNode::toFile(std::ostream &file) {
    for (const std::string &line: m_lines) {
        file << line << std::endl;
    }
}

void IniFileConfigNode::read()
{
    std::shared_ptr<std::istream> file(m_data->read());
    std::string line;
    while (getline(*file, line)) {
        m_lines.push_back(line);
    }
    m_modified = false;
}


/**
 * get property and value from line, if any present
 */
static bool getContent(const std::string &line,
                       std::string &property,
                       std::string &value,
                       bool &isComment,
                       bool fuzzyComments)
{
    size_t start = 0;
    while (start < line.size() &&
           isspace(line[start])) {
        start++;
    }

    // empty line?
    if (start == line.size()) {
        return false;
    }

    // Comment? Potentially keep reading, might be commented out assignment.
    isComment = false;
    if (line[start] == '#') {
        if (!fuzzyComments) {
            return false;
        }
        isComment = true;
    }

    // recognize # <word> = <value> as commented out (= default) value
    if (isComment) {
        start++;
        while (start < line.size() &&
               isspace(line[start])) {
            start++;
        }
    }

    // extract property
    size_t end = start;
    while (end < line.size() &&
           !isspace(line[end])) {
        end++;
    }
    property = line.substr(start, end - start);

    // skip assignment 
    start = end;
    while (start < line.size() &&
           isspace(line[start])) {
        start++;
    }
    if (start == line.size() ||
        line[start] != '=') {
        // invalid syntax or we tried to read a comment as assignment
        return false;
    }

    // extract value
    start++;
    while (start < line.size() &&
           isspace(line[start])) {
        start++;
    }

    value = line.substr(start);
    // remove trailing white space: usually it is
    // added accidentally by users
    size_t numspaces = 0;
    while (numspaces < value.size() &&
           isspace(value[value.size() - 1 - numspaces])) {
        numspaces++;
    }
    value.erase(value.size() - numspaces);

    // @TODO: strip quotation marks around value?!
    
    return true;    
}

/**
 * check whether the line contains the property and if so, extract its value
 */
static bool getValue(const std::string &line,
                     const std::string &property,
                     std::string &value,
                     bool &isComment,
                     bool fuzzyComments)

{
    std::string curProp;
    return getContent(line, curProp, value, isComment, fuzzyComments) &&
        !strcasecmp(curProp.c_str(), property.c_str());
}

InitStateString IniFileConfigNode::readProperty(const std::string &property) const
{
    std::string value;

    for (const std::string &line: m_lines) {
        bool isComment;

        if (getValue(line, property, value, isComment, false)) {
            return InitStateString(value, true);
        }
    }
    return InitStateString();
}

void IniFileConfigNode::readProperties(ConfigProps &props) const {
    std::map<std::string, std::string> res;
    std::string value, property;

    for (const std::string &line: m_lines) {
        bool isComment;
        if (getContent(line, property, value, isComment, false)) {
            // don't care about the result: only the first instance
            // of the property counts, so it doesn't matter when
            // inserting it again later fails
            props.insert(ConfigProps::value_type(property, InitStateString(value, true)));
        }
    }
}

void IniFileConfigNode::removeProperty(const std::string &property)
{
    std::string value;

    std::list<std::string>::iterator it = m_lines.begin();
    while (it != m_lines.end()) {
        const std::string &line = *it;
        bool isComment;
        if (getValue(line, property, value, isComment, false)) {
            it = m_lines.erase(it);
            m_modified = true;
        } else {
            ++it;
        }
    }
}

void IniFileConfigNode::writeProperty(const std::string &property,
                                      const InitStateString &newvalue,
                                      const std::string &comment) {
    std::string newstr;
    std::string oldvalue;
    bool isDefault = false;

    if (!newvalue.wasSet()) {
        newstr += "# ";
        isDefault = true;
    }
    newstr += property + " = " + newvalue;

    for (std::string &line: m_lines) {
        bool isComment;

        if (getValue(line, property, oldvalue, isComment, true)) {
            if (newvalue != oldvalue ||
                (isComment && !isDefault)) {
                line = newstr;
                m_modified = true;
            }
            return;
        }
    }

    // add each line of the comment as separate line in .ini file
    if (comment.size()) {
        std::list<std::string> commentLines;
        ConfigProperty::splitComment(comment, commentLines);
        if (m_lines.size()) {
            m_lines.push_back("");
        }
        for (const std::string &comment: commentLines) {
            m_lines.push_back(std::string("# ") + comment);
        }
    }

    m_lines.push_back(newstr);
    m_modified = true;
}

void IniFileConfigNode::clear()
{
    m_lines.clear();
    m_modified = true;
}

IniHashConfigNode::IniHashConfigNode(const std::shared_ptr<DataBlob> &data) :
    IniBaseConfigNode(data)
{
    read();
}

IniHashConfigNode::IniHashConfigNode(const std::string &path, const std::string &fileName, bool readonly) :
    IniBaseConfigNode(std::make_shared<FileDataBlob>(path, fileName, readonly))
{
    read();
}

void IniHashConfigNode::read()
{
    std::shared_ptr<std::istream> file(m_data->read());
    std::string line;
    while (std::getline(*file, line)) {
        std::string property, value;
        bool isComment;
        if (getContent(line, property, value, isComment, false)) {
            m_props.insert(StringPair(property, value));
        }
    }
    m_modified = false;
}

void IniHashConfigNode::toFile(std::ostream &file)
{
    for (const StringPair &prop: m_props) {
        file << prop.first << " = " <<  prop.second << std::endl;
    }
}

void IniHashConfigNode::readProperties(ConfigProps &props) const
{
    for (const StringPair &prop: m_props) {
        props.insert(ConfigProps::value_type(prop.first, InitStateString(prop.second, true)));
    }
}

void IniHashConfigNode::writeProperties(const ConfigProps &props)
{
    if (!props.empty()) {
        m_props.insert(props.begin(), props.end());
        m_modified = true;
    }
}


InitStateString IniHashConfigNode::readProperty(const std::string &property) const
{
    auto it = m_props.find(property);
    if (it != m_props.end()) {
        return InitStateString(it->second, true);
    } else {
        return InitStateString();
    }
}

void IniHashConfigNode::removeProperty(const std::string &property) {
    auto it = m_props.find(property);
    if(it != m_props.end()) {
        m_props.erase(it);
        m_modified = true;
    }
}

void IniHashConfigNode::clear()
{
    if (!m_props.empty()) {
        m_props.clear();
        m_modified = true;
    }
}

void IniHashConfigNode::writeProperty(const std::string &property,
                                      const InitStateString &newvalue,
                                      const std::string &comment)
{
    // we only store explicitly set properties
    if (!newvalue.wasSet()) {
        removeProperty(property);
        return;
    }
    auto it = m_props.find(property);
    if(it != m_props.end()) {
        if (it->second != newvalue) {
            it->second = newvalue;
            m_modified = true;
        }
    } else {
        m_props.insert(StringPair(property, newvalue));
        m_modified = true;
    }
}


SE_END_CXX
