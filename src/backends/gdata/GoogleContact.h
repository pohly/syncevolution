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

#ifndef GOOGLE_CONTACT_H
#define GOOGLE_CONTACT_H

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <utility>

#include <gdata/services/contacts/gdata-contacts-contact.h>

#include "GoogleException.h"


class XGoogleContact : public XGoogle
{
    public:
        XGoogleContact (const char *message) :
            XGoogle(message)
            { }
};


class GoogleContactString : public std::string
{
    public:
        GoogleContactString () :
            std::string()
            { }
        GoogleContactString (const GoogleContactString &src) :
            std::string(src)
            { }
        GoogleContactString (const std::string &src) :
            std::string(src)
            { }
        GoogleContactString (const char *str)
            {
                if (!str) return;
                std::string::operator=(str);
            }
        virtual ~GoogleContactString ()
            { }

        GoogleContactString & operator= (const GoogleContactString &src)
            {
                std::string::operator=(src);
                return *this;
            }
        GoogleContactString & operator= (const char *str)
            {
                if (!str) {
                    clear();
                    return *this;
                }
                std::string::operator=(str);
                return *this;
            }
        GoogleContactString & operator+= (const GoogleContactString &right)
            {
                std::string::operator+=(right);
                return *this;
            }
        GoogleContactString & operator+= (const char *str)
            {
                if (!str) return *this;
                std::string::operator+=(str);
                return *this;
            }
        GoogleContactString operator+ (const GoogleContactString &right)
            {
                std::string tmp(*this);
                return GoogleContactString(tmp + right);
            }
        GoogleContactString operator+ (const char *right)
            {
                std::string tmp(*this);
                if (!right) return tmp;
                return (tmp + right);
            }
};


class GoogleContactOrganization
{
    public:
        GoogleContactString name;
        GoogleContactString title;
        GoogleContactString role;
        GoogleContactString member;
        GoogleContactString relation;
        // TODO: logo
};


class GoogleContact
{
    public:
        GoogleContactString id;
        GoogleContactString etag;
        GoogleContactString full_name;
        GoogleContactString structured_name;
        GoogleContactString short_name;
        GoogleContactString nick_name;
        GoogleContactString birthday;
        GoogleContactString gender;
        GoogleContactString occupation;
        std::multimap<GoogleContactString, GoogleContactString> addrs;
        std::multimap<GoogleContactString, GoogleContactString> emails;
        std::multimap<GoogleContactString, GoogleContactString> phones;
        std::multimap<GoogleContactString, GoogleContactString> ims;
        std::multimap<GoogleContactString, GoogleContactString> urls;
        std::vector<GoogleContactString> langs;
        std::vector<GoogleContactOrganization> orgs;
        std::map<GoogleContactString, GoogleContactString> relations;
        // TODO: photo

        GoogleContact ();
        GoogleContact (GDataContactsContact *);
        virtual ~GoogleContact ()
            { }
};


typedef std::unique_ptr<GoogleContact> google_contact_ptr_t;
typedef std::vector<google_contact_ptr_t> google_contact_vector_t;


#endif  // GOOGLE_CONTACT_H

