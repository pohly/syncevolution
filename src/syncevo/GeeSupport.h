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

#ifndef INCL_GEE_SUPPORT
#define INCL_GEE_SUPPORT

#include <iterator>

#include <gee.h>
#include <syncevo/GLibSupport.h>

SE_GOBJECT_TYPE(GeeMap)
SE_GOBJECT_TYPE(GeeMapEntry)
SE_GOBJECT_TYPE(GeeMapIterator)
SE_GOBJECT_TYPE(GeeIterable)
SE_GOBJECT_TYPE(GeeIterator)

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * A wrapper class for some kind of Gee collection (like List or Map)
 * which provides standard const forward iterators. Main use case
 * is read-only access via BOOST_FOREACH.
 *
 * Example:
 * GeeMap *individuals = folks_individual_aggregator_get_individuals(aggregator);
 * typedef GeeCollCXX< GeeMapEntryWrapper<const gchar *, FolksIndividual *> > Coll;
 * BOOST_FOREACH (Coll::value_type &entry, Coll(individuals)) {
 *    const gchar *id = entry.key();
 *    FolksIndividual *individual(entry.value());
 *    GeeSet *emails = folks_email_details_get_email_addresses(FOLKS_EMAIL_DETAILS(individual));
 *    typedef GeeCollCXX<FolksEmailFieldDetails *> EmailColl;
 *    BOOST_FOREACH (FolksEmailFieldDetails *email, EmailColl(emails)) {
 *       const gchar *value =
 *           reinterpret_cast<const gchar *>(folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(email)));
 *    }
 * }
 *
 * @param E     the C++ type that corresponds to the entries in the collection,
 *              must be copyable and constructable from a gpointer.
 */
template<class Entry> class GeeCollCXX
{
    GeeIterableCXX m_collection;

 public:
    template<class Collection> GeeCollCXX(Collection *collection) :
        m_collection(GEE_ITERABLE(collection))
    {}

    class Iterator
    {
        mutable GeeIteratorCXX m_it;
        bool m_valid;
        Entry m_entry;

    public:
        /**
         * Takes ownership of iterator, which may be NULL for the end Iterator.
         */
        Iterator(GeeIterator *iterator) :
            m_it(iterator, false),
            m_valid(false)
        {}

        Iterator & operator ++ () {
            m_valid = gee_iterator_next(m_it);
            if (m_valid) {
                m_entry = Entry(gee_iterator_get(m_it));
            } else {
                m_entry = Entry(NULL);
            }
            return *this;
        }

        bool operator == (const Iterator &other) {
            if (other.m_it.get() == NULL) {
                // Comparison against end Iterator.
                return !m_valid;
            } else {
                // Cannot check for "point to same element";
                // at least detect when compared against ourselves.
                return this == &other;
            }
        }

        bool operator != (const Iterator &other) {
            return !(*this == other);
        }

        Entry & operator * () {
            return m_entry;
        }

        typedef Entry value_type;
        typedef ptrdiff_t difference_type;
        typedef std::forward_iterator_tag iterator_category;
        typedef Entry *pointer;
        typedef Entry &reference;
    };

    Iterator begin() const {
        Iterator it(gee_iterable_iterator(m_collection.get()));
        // advance to first element, if any
        ++it;
        return it;
    }

    Iterator end() const {
        return Iterator(NULL);
    }

    typedef Iterator const_iterator;
    typedef Iterator iterator;
    typedef Entry value_type;
};

template <class Entry> std::forward_iterator_tag iterator_category(const typename GeeCollCXX<Entry>::Iterator &) { return std::forward_iterator_tag(); }

template<class Key, class Value> class GeeMapEntryWrapper  {
    mutable GeeMapEntryCXX m_entry;
 public:
    /** take ownership of entry instance */
    GeeMapEntryWrapper(gpointer entry = NULL) :
        m_entry(reinterpret_cast<GeeMapEntry *>(entry), false)
    {}
    GeeMapEntryWrapper(const GeeMapEntryWrapper &other):
        m_entry(other.m_entry)
    {}

    Key key() const { return reinterpret_cast<Key>(const_cast<gpointer>(gee_map_entry_get_key(m_entry))); }
    Value value() const { return reinterpret_cast<Value>(const_cast<gpointer>(gee_map_entry_get_value(m_entry))); }
};

SE_END_CXX

#endif // INCL_GEE_SUPPORT
