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

class AnyContainsBoost : public IndividualFilter
{
public:
    enum Mode {
        CASE_SENSITIVE,
        CASE_INSENSITIVE
    };

    AnyContainsBoost(const std::locale &locale,
                     const std::string &searchValue,
                     Mode mode) :
        m_locale(locale),
        // m_collator(std::use_facet<boost::locale::collator>(locale)),
        m_searchValue(searchValue),
        m_mode(mode)
    {
        switch (mode) {
        case CASE_SENSITIVE:
            // Search directly, no preprocessing.
            break;
        case CASE_INSENSITIVE:
            // Locale-aware conversion to fold case (= case
            // independent) representation before search.
            m_searchValueTransformed = boost::locale::fold_case(m_searchValue, m_locale);
            break;
        }
    }

    bool containsSearchText(const char *text) const
    {
        if (!text) {
            return false;
        }
        switch (m_mode) {
        case CASE_SENSITIVE:
            return boost::contains(text, m_searchValue);
            break;
        case CASE_INSENSITIVE: {
            std::string lower(boost::locale::fold_case(text, m_locale));
            return boost::contains(lower, m_searchValueTransformed);
            break;
        }
        }
        // not reached
        return false;
    }

    virtual bool matches(FolksIndividual *individual) const
    {
        FolksNameDetails *name = FOLKS_NAME_DETAILS(individual);
        const char *fullname = folks_name_details_get_full_name(name);
        if (containsSearchText(fullname)) {
            return true;
        }
        const char *nickname = folks_name_details_get_nickname(name);
        if (containsSearchText(nickname)) {
            return true;
        }
        FolksStructuredName *fn =
            folks_name_details_get_structured_name(FOLKS_NAME_DETAILS(individual));
        if (fn) {
            const char *given = folks_structured_name_get_given_name(fn);
            if (containsSearchText(given)) {
                return true;
            }
            const char *middle = folks_structured_name_get_additional_names(fn);
            if (containsSearchText(middle)) {
                return true;
            }
            const char *family = folks_structured_name_get_family_name(fn);
            if (containsSearchText(family)) {
                return true;
            }
        }
        FolksEmailDetails *emailDetails = FOLKS_EMAIL_DETAILS(individual);
        GeeSet *emails = folks_email_details_get_email_addresses(emailDetails);
        BOOST_FOREACH (FolksEmailFieldDetails *email, GeeCollCXX<FolksEmailFieldDetails *>(emails)) {
            const gchar *value =
                reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(email)));
            if (containsSearchText(value)) {
                return true;
            }
        }
        return false;
    }

private:
    std::locale m_locale;
    std::string m_searchValue;
    std::string m_searchValueTransformed;
    Mode m_mode;
    // const bool (*m_contains)(const std::string &, const std::string &, const std::locale &);
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

    virtual boost::shared_ptr<IndividualFilter> createFilter(const Filter_t &filter)
    {
        boost::shared_ptr<IndividualFilter> res;
        if (filter.size() != 1) {
            SE_THROW(StringPrintf("boost locale factory: only filter with one term are supported (was given %ld)",
                                  (long)filter.size()));
        }
        const Filter_t::value_type &term = filter[0];
        if (term.empty()) {
            SE_THROW("boost locale factory: empty search term not supported");
        }
        if (term[0] == "any-contains") {
            AnyContainsBoost::Mode mode = AnyContainsBoost::CASE_INSENSITIVE;
            if (term.size() < 2) {
                SE_THROW("boost locale factory: any-contains search needs one parameter");
            }
            for (size_t i = 2; i < term.size(); i++) {
                const std::string &flag = term[i];
                if (flag == "case-sensitive") {
                    mode = AnyContainsBoost::CASE_SENSITIVE;
                } else if (flag == "case-insensitive") {
                    mode = AnyContainsBoost::CASE_INSENSITIVE;
                } else {
                    SE_THROW("boost locale factory: unknown flag for any-contains: " + flag);
                }
            }
            res.reset(new AnyContainsBoost(m_locale, term[1], mode));

            // TODO: combine with phone number lookup
        } else if (term[0] == "phone") {
            // TODO
            SE_THROW("boost locale factor: phone number lookup not implemented");
        } else {
            SE_THROW("boost locale factory: unknown search term: " + term[0]);
        }

        return res;
    }
};

boost::shared_ptr<LocaleFactory> LocaleFactory::createFactory()
{
    return boost::shared_ptr<LocaleFactory>(new LocaleFactoryBoost());
}

SE_END_CXX
