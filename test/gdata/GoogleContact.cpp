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

#include <glib.h>
#include <glib/gprintf.h>

#include "GoogleContact.h"


static const char * _pref_prefix = "PREF=1:";
static const char * _empty = "";
static const char * _home = "TYPE=home";
static const char * _work = "TYPE=work";


static std::pair<GoogleContactString, GoogleContactString> _convert_postal (
    GDataGDPostalAddress *addr)
{
    GoogleContactString semicolon(";");
    GoogleContactString str;
    std::pair<GoogleContactString, GoogleContactString> pair;
    const gchar *sptr;

    if (addr) {
        str += gdata_gd_postal_address_get_po_box(addr);
        str += semicolon + gdata_gd_postal_address_get_agent(addr);
        str += semicolon + gdata_gd_postal_address_get_street(addr);
        str += semicolon + gdata_gd_postal_address_get_city(addr);
        str += semicolon + gdata_gd_postal_address_get_region(addr);
        str += semicolon + gdata_gd_postal_address_get_postcode(addr);
        str += semicolon + gdata_gd_postal_address_get_country(addr);

        sptr = gdata_gd_postal_address_get_relation_type(addr);
        if (!sptr) {
            sptr = gdata_gd_postal_address_get_label(addr);
        }
        if (g_strcmp0(sptr, GDATA_GD_POSTAL_ADDRESS_WORK) == 0) {
            sptr = _work;
        } else if (g_strcmp0(sptr, GDATA_GD_POSTAL_ADDRESS_HOME) == 0) {
            sptr = _home;
        } else {
            sptr = _empty;
        }
        pair = std::make_pair(str, GoogleContactString(sptr));
    }

    return pair;
}


GoogleContact::GoogleContact ()
{
}


GoogleContact::GoogleContact (GDataContactsContact *contact)
{
    gchar *str;
    const gchar *sptr;
    GList *list;
    GDataGDName *name;
    GDate *date;

    // full name
    name = gdata_contacts_contact_get_name(contact);
    sptr = gdata_gd_name_get_full_name(name);
    if (sptr) {
        full_name = sptr;
    }

    // structured name
    sptr = gdata_gd_name_get_family_name(name);
    const gchar *name_family = (sptr) ? sptr : _empty;
    sptr = gdata_gd_name_get_given_name(name);
    const gchar *name_given = (sptr) ? sptr : _empty;
    sptr = gdata_gd_name_get_additional_name(name);
    const gchar *name_middle = (sptr) ? sptr : _empty;
    sptr = gdata_gd_name_get_prefix(name);
    const gchar *name_prefix = (sptr) ? sptr : _empty;
    sptr = gdata_gd_name_get_suffix(name);
    const gchar *name_suffix = (sptr) ? sptr : _empty;
    str = g_strdup_printf("%s;%s;%s;%s;%s",
                          name_family,
                          name_given,
                          name_middle,
                          name_prefix,
                          name_suffix);
    if (str) {
        structured_name = str;
        g_free(str);
    }

    // short name
    short_name = gdata_contacts_contact_get_short_name(contact);

    // nickname
    nick_name = gdata_contacts_contact_get_nickname(contact);

    // birthday
    date = g_date_new();
    gboolean yearvalid = gdata_contacts_contact_get_birthday(contact, date);
    if (g_date_valid(date)) {
        if (yearvalid) {
            gchar datestr[4 + 2 + 2 + 1];

            g_date_strftime(datestr, sizeof(datestr), "%Y%m%d", date);
            birthday = datestr;
        } else {
            gchar datestr[2 + 2 + 2 + 1];

            g_date_strftime(datestr, sizeof(datestr), "--%m%d", date);
            birthday = datestr;
        }
    }
    g_date_free(date);

    // gender
    sptr = gdata_contacts_contact_get_gender(contact);
    if (sptr) {
        // TODO: translate to vCard M/F/O/N/U;[identity] format once
        // Google's spec is known
        gender = sptr;
    }

    // occupation (JOB in vCard)
    occupation = gdata_contacts_contact_get_occupation(contact);

    // postal addresses
    for (list = gdata_contacts_contact_get_postal_addresses(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGDPostalAddress *addr = GDATA_GD_POSTAL_ADDRESS(list->data);
        std::pair<GoogleContactString, GoogleContactString> paddr =
            _convert_postal(addr);
        if (gdata_gd_postal_address_is_primary(addr)) {
            paddr.first = GoogleContactString(_pref_prefix) + paddr.first;
        }
        addrs.insert(paddr);
    }

    // email addresses
    for (list = gdata_contacts_contact_get_email_addresses(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGDEmailAddress *email = GDATA_GD_EMAIL_ADDRESS(list->data);
        sptr = gdata_gd_email_address_get_relation_type(email);
        if (!sptr) {
            sptr = gdata_gd_email_address_get_label(email);
        }
        if (g_strcmp0(sptr, GDATA_GD_EMAIL_ADDRESS_HOME) == 0) {
            sptr = _home;
        } else if (g_strcmp0(sptr, GDATA_GD_EMAIL_ADDRESS_WORK) == 0) {
            sptr = _work;
        } else {
            sptr = _empty;
        }
        std::pair<GoogleContactString, GoogleContactString> eaddr =
            std::make_pair(
                GoogleContactString(gdata_gd_email_address_get_address(email)),
                GoogleContactString(sptr));
        if (gdata_gd_email_address_is_primary(email)) {
            eaddr.first = GoogleContactString(_pref_prefix) + eaddr.first;
        }
        emails.insert(eaddr);
    }

    // phone numbers
    for (list = gdata_contacts_contact_get_phone_numbers(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGDPhoneNumber *phone = GDATA_GD_PHONE_NUMBER(list->data);
        sptr = gdata_gd_phone_number_get_relation_type(phone);
        if (!sptr) {
            sptr = gdata_gd_phone_number_get_label(phone);
        }
        if (g_strcmp0(sptr, GDATA_GD_PHONE_NUMBER_HOME) == 0) {
            sptr = _home;
        } else if (g_strcmp0(sptr, GDATA_GD_PHONE_NUMBER_WORK) == 0) {
            sptr = _work;
        } else {
            sptr = _empty;
        }
        std::pair<GoogleContactString, GoogleContactString> lphone =
            std::make_pair(
                GoogleContactString(gdata_gd_phone_number_get_number(phone)),
                GoogleContactString(sptr));
        if (gdata_gd_phone_number_is_primary(phone)) {
            lphone.first = GoogleContactString(_pref_prefix) + lphone.first;
        }
        phones.insert(lphone);
    }

    // instant messaging addresses
    for (list = gdata_contacts_contact_get_im_addresses(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGDIMAddress *imaddr = GDATA_GD_IM_ADDRESS(list->data);
        sptr = gdata_gd_im_address_get_relation_type(imaddr);
        if (!sptr) {
            sptr = gdata_gd_im_address_get_label(imaddr);
        }
        if (g_strcmp0(sptr, GDATA_GD_IM_ADDRESS_HOME) == 0) {
            sptr = _home;
        } else if (g_strcmp0(sptr, GDATA_GD_IM_ADDRESS_WORK) == 0) {
            sptr = _work;
        } else {
            sptr = _empty;
        }
        const gchar *proto = gdata_gd_im_address_get_protocol(imaddr);
        if (g_strcmp0(proto, GDATA_GD_IM_PROTOCOL_GOOGLE_TALK) == 0 ||
            g_strcmp0(proto, GDATA_GD_IM_PROTOCOL_JABBER) == 0) {
            proto = "xmpp:";
        } else if (g_strcmp0(proto, GDATA_GD_IM_PROTOCOL_AIM) == 0) {
            proto = "aim:";
        } else if (g_strcmp0(proto, GDATA_GD_IM_PROTOCOL_LIVE_MESSENGER) == 0) {
            proto = "msnim:";
        } else if (g_strcmp0(proto,
            GDATA_GD_IM_PROTOCOL_YAHOO_MESSENGER) == 0) {
            proto = "ymsgr:";
        } else if (g_strcmp0(proto, GDATA_GD_IM_PROTOCOL_SKYPE) == 0) {
            proto = "skype:";  // or callto
        } else if (g_strcmp0(proto, GDATA_GD_IM_PROTOCOL_QQ) == 0) {
            proto = "qq:";  // NOTE: no official URI scheme?
        } else if (g_strcmp0(proto, GDATA_GD_IM_PROTOCOL_ICQ) == 0) {
            proto = "icq:";
        }
        GoogleContactString imuri =
            GoogleContactString(proto) +
            GoogleContactString(gdata_gd_im_address_get_address(imaddr));
        std::pair<GoogleContactString, GoogleContactString> im =
            std::make_pair(
                imuri,
                GoogleContactString(sptr));
        if (gdata_gd_im_address_is_primary(imaddr)) {
            im.first = GoogleContactString(_pref_prefix) + im.first;
        }
        ims.insert(im);
    }

    // languages
    for (list = gdata_contacts_contact_get_languages(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGContactLanguage *lang = GDATA_GCONTACT_LANGUAGE(list->data);
        sptr = gdata_gcontact_language_get_code(lang);
        langs.push_back(GoogleContactString(sptr));
    }

    // organizations
    for (list = gdata_contacts_contact_get_organizations(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGDOrganization *org = GDATA_GD_ORGANIZATION(list->data);
        GoogleContactOrganization lorg;
        lorg.name = gdata_gd_organization_get_name(org);
        lorg.title = gdata_gd_organization_get_title(org);
        lorg.role = gdata_gd_organization_get_job_description(org);
        lorg.member = gdata_gd_organization_get_department(org);
        sptr = gdata_gd_organization_get_relation_type(org);
        if (g_strcmp0(sptr, GDATA_GD_ORGANIZATION_WORK) == 0) {
            lorg.relation = "work";
        } else {
            lorg.relation = gdata_gd_organization_get_label(org);
        }
        if (gdata_gd_organization_is_primary(org)) {
            lorg.name = GoogleContactString(_pref_prefix) + lorg.name;
        }
        orgs.push_back(lorg);
    }

    // urls
    for (list = gdata_contacts_contact_get_websites(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGContactWebsite *web = GDATA_GCONTACT_WEBSITE(list->data);
        sptr = gdata_gcontact_website_get_relation_type(web);
        if (!sptr) {
            sptr = gdata_gcontact_website_get_label(web);
        }
        if (g_strcmp0(sptr, GDATA_GCONTACT_WEBSITE_HOME_PAGE) == 0 ||
            g_strcmp0(sptr, GDATA_GCONTACT_WEBSITE_HOME) == 0) {
            sptr = _home;
        } else if (g_strcmp0(sptr, GDATA_GCONTACT_WEBSITE_WORK) == 0) {
            sptr = _work;
        } else {
            sptr = _empty;
        }
        std::pair<GoogleContactString, GoogleContactString> url =
            std::make_pair(
                GoogleContactString(gdata_gcontact_website_get_uri(web)),
                GoogleContactString(sptr));
        if (gdata_gcontact_website_is_primary(web)) {
            url.first = GoogleContactString(_pref_prefix) + url.first;
        }
        urls.insert(url);
    }

    // relations
    for (list = gdata_contacts_contact_get_relations(contact);
         list != NULL;
         list = g_list_next(list)) {
        GDataGContactRelation *relation = GDATA_GCONTACT_RELATION(list->data);
        sptr = gdata_gcontact_relation_get_relation_type(relation);
        if (!sptr) {
            sptr = gdata_gcontact_relation_get_label(relation);
        }
        // TODO: translate relation data
        std::pair<GoogleContactString, GoogleContactString> related =
            std::make_pair(
                GoogleContactString(gdata_gcontact_relation_get_name(relation)),
                GoogleContactString(sptr));
        relations.insert(related);
    }
}

