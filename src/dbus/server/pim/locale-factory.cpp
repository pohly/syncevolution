/*
 * Copyright (C) 2012 Intel Corporation
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

/**
 * Common code for sorting and searching.
 */

#include "locale-factory.h"
#include "folks.h"

#include <boost/lexical_cast.hpp>
#include <sstream>

SE_BEGIN_CXX

std::string SimpleE164::toString() const
{
    std::ostringstream out;
    if (m_countryCode) {
        out << "+" << m_countryCode;
    }
    if (m_nationalNumber) {
        out << m_nationalNumber;
    }
    return out.str();
}

class Filter2StringVisitor : public boost::static_visitor<void>
{
    std::ostringstream m_out;

public:
    void operator () (const std::string &str)
    {
        m_out << "'" << str << "'";
    }

    void operator () (const std::vector<LocaleFactory::Filter_t> &filter)
    {
        m_out << "[";
        for (size_t i = 0; i < filter.size(); i++) {
            if (i == 0) {
                m_out << " ";
            } else {
                m_out << ", ";
            }
            boost::apply_visitor(*this, filter[i]);
        }
        m_out << " ]";
    }

    std::string toString() { return m_out.str(); }
};

std::string LocaleFactory::Filter2String(const Filter_t &filter)
{
    Filter2StringVisitor visitor;
    boost::apply_visitor(visitor, filter);
    return visitor.toString();
}

template <class V> const V &getFilter(const LocaleFactory::Filter_t &filter, const char *expected)
{
    const V *value = boost::get<V>(&filter);
    if (!value) {
        throw std::runtime_error(StringPrintf("expected %s, got instead: %s",
                                              expected, LocaleFactory::Filter2String(filter).c_str()));
    }
    return *value;
}

const std::string &LocaleFactory::getFilterString(const Filter_t &filter, const char *expected)
{
    return getFilter<std::string>(filter, expected);
}
const std::vector<LocaleFactory::Filter_t> &LocaleFactory::getFilterArray(const Filter_t &filter, const char *expected)
{
    return getFilter< std::vector<Filter_t> >(filter, expected);
}

void LocaleFactory::handleFilterException(const Filter_t &filter, int level, const std::string *file, int line)
{
    std::string what;
    Exception::handle(what, HANDLE_EXCEPTION_NO_ERROR);
    what = StringPrintf("%s   nesting level %d: %s\n%s",
                        level == 0 ? "Error while parsing a search filter.\nMost specific term comes last, then the error message:\n" : "",
                        level,
                        Filter2String(filter).c_str(),
                        what.c_str());
    if (file) {
        throw Exception(*file, line, what);
    } else {
        throw std::runtime_error(what);
    }
}

class LogicFilter : public IndividualFilter
{
protected:
    std::vector< boost::shared_ptr<IndividualFilter> > m_subFilter;
public:
    void addFilter(const boost::shared_ptr<IndividualFilter> &filter) { m_subFilter.push_back(filter); }
};

class OrFilter : public LogicFilter
{
public:
    virtual bool matches(const IndividualData &data) const
    {
        BOOST_FOREACH (const boost::shared_ptr<IndividualFilter> &filter, m_subFilter) {
            if (filter->matches(data)) {
                return true;
            }
        }
        return false;
    }
};

class AndFilter : public LogicFilter
{
public:
    virtual bool matches(const IndividualData &data) const
    {
        BOOST_FOREACH (const boost::shared_ptr<IndividualFilter> &filter, m_subFilter) {
            if (!filter->matches(data)) {
                return false;
            }
        }

        // Does not match if empty, just like 'or'.
        return !m_subFilter.empty();
    }
};

boost::shared_ptr<IndividualFilter> LocaleFactory::createFilter(const Filter_t &filter, int level)
{
    boost::shared_ptr<IndividualFilter> res;

    try {
        const std::vector<Filter_t> &terms = getFilterArray(filter, "array of terms");

        if (terms.empty()) {
            res.reset(new MatchAll());
            return res;
        }

        // Array of arrays?
        // May contain search parameters ('limit') and one
        // filter expression.
        if (boost::get< std::vector<Filter_t> >(&terms[0])) {
            boost::shared_ptr<IndividualFilter> params;
            BOOST_FOREACH (const Filter_t &subfilter, terms) {
                boost::shared_ptr<IndividualFilter> tmp = createFilter(subfilter, level + 1);
                if (dynamic_cast<ParamFilter *>(tmp.get())) {
                    // New parameter overwrites old one. If we ever
                    // want to support more than one parameter, we
                    // need to be more selective here.
                    params = tmp;
                } else if (!res) {
                    res = tmp;
                } else {
                    SE_THROW("Filter can only be combined with other filters inside a logical operation.");
                }
            }
            if (params) {
                if (res) {
                    // Copy parameter(s) to real filter.
                    res->setMaxResults(params->getMaxResults());
                } else {
                    // Or just use it as-is because no filter was
                    // given. It'll work like MatchAll.
                    res = params;
                }
            }
        } else {
            // Not an array, so must be string.
            const std::string &operation = getFilterString(terms[0], "operation name");

            if (operation == "limit") {
                // Level 0 is the [] containing the ['limit', ...].
                // We thus expect it at level 1.
                if (level != 1) {
                    SE_THROW("'limit' parameter only allowed at top level.");
                }
                if (terms.size() != 2) {
                    SE_THROW("'limit' needs exactly one parameter.");
                }
                const std::string &limit = getFilterString(terms[1], "'filter' value as string");
                int maxResults = boost::lexical_cast<int>(limit);
                res.reset(new ParamFilter());
                res->setMaxResults(maxResults);
            } else if (operation == "or" || operation == "and") {
                boost::shared_ptr<LogicFilter> logicFilter(operation == "or" ?
                                                           static_cast<LogicFilter *>(new OrFilter()) :
                                                           static_cast<LogicFilter *>(new AndFilter()));
                for (size_t i = 1; i < terms.size(); i++ ) {
                    logicFilter->addFilter(createFilter(terms[i], level + 1));
                }
                res = logicFilter;
            } else {
                SE_THROW(StringPrintf("Unknown operation '%s'", operation.c_str()));
            }
        }
    } catch (const Exception &ex) {
        handleFilterException(filter, level, &ex.m_file, ex.m_line);
    } catch (...) {
        handleFilterException(filter, level, NULL, 0);
    }

    return res;
}

SE_END_CXX
