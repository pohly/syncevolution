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

#include "GoogleVCard.h"


static const char * _eol = "\r\n";


GoogleVCard::GoogleVCard (GoogleContact &contact)
{
    std::multimap<GoogleContactString, GoogleContactString>::iterator sm_iter;
    std::vector<GoogleContactString>::iterator sv_iter;
    std::vector<GoogleContactOrganization>::iterator so_iter;

    card += "BEGIN:VCARD\r\n";
    card += "VERSION:4.0\r\n";
    card += "KIND:individual\r\n";

    if (!contact.full_name.empty())
        card += "FN:" + contact.full_name + _eol;
    if (!contact.structured_name.empty())
        card += "N:" + contact.structured_name + _eol;
    if (!contact.nick_name.empty())
        card += "NICKNAME:" + contact.nick_name + _eol;
    if (!contact.birthday.empty())
        card += "BDAY:" + contact.birthday + _eol;
    // TODO: check
    if (!contact.gender.empty())
        card += "GENDER:" + contact.gender + _eol;
    for (sm_iter = contact.addrs.begin();
         sm_iter != contact.addrs.end();
         sm_iter++) {
        card += "ADR:";
        if (!sm_iter->second.empty())
            card += sm_iter->second + ":";
        card += sm_iter->first + _eol;
    }
    for (sm_iter = contact.phones.begin();
         sm_iter != contact.phones.end();
         sm_iter++) {
        card += "TEL:";
        if (!sm_iter->second.empty())
            card += sm_iter->second + ":";
        card += sm_iter->first + _eol;
    }
    for (sm_iter = contact.emails.begin();
         sm_iter != contact.emails.end();
         sm_iter++) {
        card += "EMAIL:";
        if (!sm_iter->second.empty())
            card += sm_iter->second + ":";
        card += sm_iter->first + _eol;
    }
    for (sm_iter = contact.ims.begin();
         sm_iter != contact.ims.end();
         sm_iter++) {
        card += "IMS:";
        if (!sm_iter->second.empty())
            card += sm_iter->second + ":";
        card += sm_iter->first + _eol;
    }
    for (sv_iter = contact.langs.begin();
         sv_iter != contact.langs.end();
         sv_iter++) {
        card += "LANG:" + *sv_iter + _eol;
    }
    for (so_iter = contact.orgs.begin();
         so_iter != contact.orgs.end();
         so_iter++) {
        if (!so_iter->title.empty())
            card += "TITLE:" + so_iter->title + _eol;
        if (!so_iter->role.empty())
            card += "ROLE:" + so_iter->role + _eol;
        if (!so_iter->name.empty())
            card += "ORG:" + so_iter->name + _eol;
        if (!so_iter->member.empty())
            card += "MEMBER:" + so_iter->member + _eol;
    }
    for (sm_iter = contact.urls.begin();
         sm_iter != contact.urls.end();
         sm_iter++) {
        card += "URL:";
        if (!sm_iter->second.empty())
            card += sm_iter->second + ":";
        card += sm_iter->first + _eol;
    }

    card += "END:VCARD\r\n";
}

