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
SE_GOBJECT_TYPE(GeeMultiMap)
SE_GOBJECT_TYPE(GeeCollection)

#include <syncevo/declarations.h>
SE_BEGIN_CXX

namespace GeeSupport {
    /** Used for GeeMapEntryWrapper. */
    template<class E> struct traits {
        typedef E Wrapper_t;
        typedef typename E::Cast_t Cast_t;
        static E get(Wrapper_t &wrapper) { return wrapper; }
    };

    /** Default is for types which have a corresponding SE_GOBJECT_TYPE. */
    template<class E> struct traits<E *> {
        typedef StealGObject<E> Wrapper_t;
        typedef E * Cast_t;
        static E * get(Wrapper_t &wrapper) { return wrapper.get(); }
    };

    /** Dynamically allocated plain C strings also work. */
    template<> struct traits<const gchar *> {
        typedef PlainGStr Wrapper_t;
        typedef gchar * Cast_t;
        static const gchar * get(Wrapper_t &wrapper) { return wrapper; }
    };
}

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
 * @param Entry The C++ type that corresponds to the entries in the collection,
 *              must be copyable and constructable from a gpointer (default) or
 *              intermediate type Cast (when given). Must own the content
 *              pointed to by the gpointer. Plain pointers are not good enough,
 *              they lead to memory leaks!
 * @param Cast  Used to cast gpointer into something that Entry's constructor accepts.
 */
template<class Entry> class GeeCollCXX
{
    GeeIterableCXX m_collection;

 public:
    template<class Collection> GeeCollCXX(Collection *collection, RefOwnership ownership) :
        m_collection(GEE_ITERABLE(collection), ownership)
    {}

    GeeCollCXX(GeeCollectionCXX &collection) :
        m_collection(GEE_ITERABLE(collection.get()), ADD_REF)
    {}

    GeeIterable *get() const { return m_collection.get(); }

    class Iterator
    {
        /** Defines how to handle the gpointer result of gee_iterator_get(). */
        typedef GeeSupport::traits<Entry> Traits_t;

        mutable GeeIteratorCXX m_it;
        bool m_valid;
        /** A smart pointer which owns the value returned by gee_iterator_get(). */
        typename Traits_t::Wrapper_t m_wrapper;
        /** A copy of the wrapped value, needed because the * operator must return a reference to it. */
        Entry m_entry;
        

    public:
        /**
         * Takes ownership of iterator, which may be NULL for the end Iterator.
         */
        Iterator(GeeIterator *iterator) :
            m_it(iterator, TRANSFER_REF),
            m_valid(false)
        {}

        Iterator & operator ++ () {
            m_valid = gee_iterator_next(m_it);
            if (m_valid) {
                // First cast gpointer into something which is accepted by the wrapper. */
                m_wrapper = typename Traits_t::Wrapper_t(static_cast<typename Traits_t::Cast_t>((gee_iterator_get(m_it))));
                m_entry = Traits_t::get(m_wrapper);
            } else {
                m_wrapper = typename Traits_t::Wrapper_t(NULL);
                m_entry = NULL;
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

/** A collection of gchar * strings. */
typedef GeeCollCXX<const gchar *> GeeStringCollection;

template <class Entry> std::forward_iterator_tag iterator_category(const typename GeeCollCXX<Entry>::Iterator &) { return std::forward_iterator_tag(); }

template<class Key, class Value> class GeeMapEntryWrapper  {
    mutable GeeMapEntryCXX m_entry;
 public:
    typedef GeeMapEntry *Cast_t;

    /** take ownership of entry instance */
    GeeMapEntryWrapper(GeeMapEntry *entry = NULL) :
        m_entry(entry, TRANSFER_REF)
    {}
    GeeMapEntryWrapper(const GeeMapEntryWrapper &other):
        m_entry(other.m_entry)
    {}

    Key key() const { return reinterpret_cast<Key>(const_cast<gpointer>(gee_map_entry_get_key(m_entry))); }
    Value value() const { return reinterpret_cast<Value>(const_cast<gpointer>(gee_map_entry_get_value(m_entry))); }
};

SE_END_CXX

#endif // INCL_GEE_SUPPORT
