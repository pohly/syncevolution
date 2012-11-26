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
 * Abstract definition of sorting and searching plugin. Used
 * by folks.cpp, must be provided by exactly one implementation
 * which is chosen at compile time.
 */

#ifndef INCL_SYNCEVO_DBUS_SERVER_PIM_LOCALE_FACTORY
#define INCL_SYNCEVO_DBUS_SERVER_PIM_LOCALE_FACTORY

#include <boost/shared_ptr.hpp>

#include <folks/folks.h>

#include <syncevo/util.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

class IndividualCompare;
class IndividualFilter;


/**
 * Factory for everything related to the current locale: sorting and
 * searching.
 */
class LocaleFactory
{
 public:
    /**
     * Exactly one factory can be created, chosen at compile time.
     */
    static boost::shared_ptr<LocaleFactory> createFactory();

    /**
     * Creates a compare instance or throws an error when that is not
     * possible.
     *
     * @param order     factory-specific string which chooses one of
     *                  the orderings supported by the factory
     * @return a valid instance, must not be NULL
     */
    virtual boost::shared_ptr<IndividualCompare> createCompare(const std::string &order) = 0;

    /**
     * An array of search terms which all must match. Each search term
     * itself is again an array of strings, with the first one choosing
     * the search criteria and the rest providing parameters for that
     * search term.
     */
    typedef std::vector< std::vector<std::string> > Filter_t;

    /**
     * Creates a filter instance or throws an error when that is not
     * possible.
     *
     * @param order     factory-specific string which chooses one of
     *                  the search criteria supported by the factory
     * @return a valid instance, must not be NULL
     */
    virtual boost::shared_ptr<IndividualFilter> createFilter(const Filter_t &filter) = 0;

    /**
     * Pre-computed data for a single FolksIndividual which will be needed
     * for searching. Strictly speaking, this should be an opaque pointer
     * whose content is entirely owned by the implementation of LocaleFactory.
     * For the sake of performance and simplicity, we define a struct instead
     * which can be embedded inside IndividualData. Leads to better memory
     * locality and reduces overall memory consumption/usage.
     */
    struct Precomputed
    {
        /**
         * Normalized phone numbers (E164). Contains only + and digits.
         * TODO (?): store in more compact format.
         */
        std::vector<std::string> m_phoneNumbers;
    };

    /**
     * (Re)set pre-computed data for an individual.
     */
    virtual void precompute(FolksIndividual *individual, Precomputed &precomputed) const = 0;
};

SE_END_CXX

#endif // INCL_SYNCEVO_DBUS_SERVER_PIM_LOCALE_FACTORY
