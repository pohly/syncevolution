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

#include <syncevo/PrefixConfigNode.h>
#include <syncevo/Exception.h>

#include <boost/algorithm/string/predicate.hpp>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

PrefixConfigNode::PrefixConfigNode(const std::string prefix,
                                   const std::shared_ptr<ConfigNode> &node) :
    m_prefix(prefix),
    m_node(node),
    m_readOnlyNode(node)
{
}

PrefixConfigNode::PrefixConfigNode(const std::string prefix,
                                   const std::shared_ptr<const ConfigNode> &node) :
    m_prefix(prefix),
    m_readOnlyNode(node)
{
}

InitStateString PrefixConfigNode::readProperty(const std::string &property) const
{
    return m_readOnlyNode->readProperty(m_prefix + property);
}

void PrefixConfigNode::writeProperty(const std::string &property,
                                     const InitStateString &value,
                                     const std::string &comment)
{
    m_node->writeProperty(m_prefix + property,
                          value,
                          comment);
}

void PrefixConfigNode::readProperties(ConfigProps &props) const
{
    ConfigProps original;
    m_readOnlyNode->readProperties(original);

    for (const auto &prop: original) {
        std::string key = prop.first;
        std::string value = prop.second;

        if (boost::starts_with(key, m_prefix)) {
            props[key.substr(m_prefix.size())] = value;
        }
    }
}

void PrefixConfigNode::clear()
{
    ConfigProps original;
    m_readOnlyNode->readProperties(original);
    for (const auto &prop: original) {
        std::string key = prop.first;
        if (boost::starts_with(key, m_prefix)) {
            m_node->removeProperty(key);
        }
    }
}

void PrefixConfigNode::removeProperty(const std::string &property)
{
    m_node->removeProperty(m_prefix + property);
}

void PrefixConfigNode::flush()
{
    if (!m_node.get()) {
        Exception::throwError(SE_HERE, getName() + ": read-only, flushing not allowed");
    }
    m_node->flush();
}

SE_END_CXX
