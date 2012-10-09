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
 * The boost::locale based implementation of locale-factory.h.
 */

#include "locale-factory.h"
#include "folks.h"

#include <boost/locale.hpp>

SE_BEGIN_CXX

/**
 * Use higher levels to break ties between strings which are
 * considered equal at the lower levels. For example, "Façade" and
 * "facade" would be compared as equal when using only base
 * characters, but compare differently when also considering a higher
 * level which includes accents.
 *
 * The drawback of higher levels is that they are computationally more
 * expensive (transformation is slower and leads to longer transformed
 * strings, thus a longer string comparisons during compare) and may
 * end up comparing aspects that are irrelevant (like case).
 *
 * The default here pays attention to accents, but ignores the case.
 */
static const boost::locale::collator_base::level_type DEFAULT_COLLATION_LEVEL = boost::locale::collator_base::secondary;

class CompareFirstLastBoost : public IndividualCompare {
    std::locale m_locale;
    const boost::locale::collator<char> &m_collator;
public:
    CompareFirstLastBoost(const std::locale &locale) :
        m_locale(locale),
        m_collator(std::use_facet< boost::locale::collator<char> >(m_locale))
    {}

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *family = folks_structured_name_get_family_name(fn);
            const char *given = folks_structured_name_get_given_name(fn);
            criteria.push_back(given ? m_collator.transform(DEFAULT_COLLATION_LEVEL, given) : "");
            criteria.push_back(family ? m_collator.transform(DEFAULT_COLLATION_LEVEL, family) : "");
        }
    }
};

class CompareLastFirstBoost : public IndividualCompare {
    std::locale m_locale;
    const boost::locale::collator<char> &m_collator;
public:
    CompareLastFirstBoost(const std::locale &locale) :
        m_locale(locale),
        m_collator(std::use_facet< boost::locale::collator<char> >(m_locale))
    {}

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *family = folks_structured_name_get_family_name(fn);
            const char *given = folks_structured_name_get_given_name(fn);
            criteria.push_back(family ? m_collator.transform(DEFAULT_COLLATION_LEVEL, family) : "");
            criteria.push_back(given ? m_collator.transform(DEFAULT_COLLATION_LEVEL, given) : "");
        }
    }
};

class CompareFullnameBoost : public IndividualCompare {
    std::locale m_locale;
    const boost::locale::collator<char> &m_collator;
public:
    CompareFullnameBoost(const std::locale &locale) :
        m_locale(locale),
        m_collator(std::use_facet< boost::locale::collator<char> >(m_locale))
    {}

    virtual void createCriteria(FolksIndividual *individual, Criteria_t &criteria) const {
        const char *fullname = folks_name_details_get_full_name(FOLKS_NAME_DETAILS(individual));
        if (fullname) {
            criteria.push_back(m_collator.transform(DEFAULT_COLLATION_LEVEL, fullname));
        } else {
            FolksStructuredName *fn =
                folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
            if (fn) {
                const char *given = folks_structured_name_get_given_name(fn);
                const char *middle = folks_structured_name_get_additional_names(fn);
                const char *family = folks_structured_name_get_family_name(fn);
                const char *suffix = folks_structured_name_get_suffixes(fn);
                std::string buffer;
                buffer.reserve(256);
#define APPEND(_str) \
                if (_str && *_str) { \
                    if (!buffer.empty()) { \
                        buffer += _str; \
                    } \
                }
                APPEND(given);
                APPEND(middle);
                APPEND(family);
                APPEND(suffix);
#undef APPEND
                criteria.push_back(m_collator.transform(DEFAULT_COLLATION_LEVEL, buffer));
            }
        }
    }
};

class LocaleFactoryBoost : public LocaleFactory
{
    std::locale m_locale;

public:
    LocaleFactoryBoost() :
        m_locale(genLocale())
    {}

    static std::locale genLocale()
    {
        // Get current locale from environment. Configure the
        // generated locale so that it supports what we need and
        // nothing more.
        boost::locale::generator gen;
        gen.characters(boost::locale::char_facet);
        gen.categories(boost::locale::collation_facet |
                       boost::locale::convert_facet |
                       boost::locale::information_facet);
        // TODO: Check env vars, then append @collation=phonebook
        // to get phonebook specific sorting.
        return gen("");
    }

    virtual boost::shared_ptr<IndividualCompare> createCompare(const std::string &order)
    {
        boost::shared_ptr<IndividualCompare> res;
        if (order == "first/last") {
            res.reset(new CompareFirstLastBoost(m_locale));
        } else if (order == "last/first") {
            res.reset(new CompareLastFirstBoost(m_locale));
        } else if (order == "fullname") {
            res.reset(new CompareFullnameBoost(m_locale));
        } else {
            SE_THROW("boost locale factory: sort order '" + order + "' not supported");
        }
        return res;
    }

    virtual boost::shared_ptr<IndividualFilter> createFilter(const StringMap &filter)
    {
        SE_THROW("not implemented");
    }
};

boost::shared_ptr<LocaleFactory> LocaleFactory::createFactory()
{
    return boost::shared_ptr<LocaleFactory>(new LocaleFactoryBoost());
}

SE_END_CXX
