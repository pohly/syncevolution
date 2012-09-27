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
 * The D-Bus IPC binding for folks.h. Maps FolksIndividual to and
 * from the D-Bus dict described in pim-manager-api.txt.
 */

#include "individual-traits.h"
#include "folks.h"

SE_GLIB_TYPE(GDateTime, g_date_time)

SE_BEGIN_CXX

static const char * const CONTACT_HASH_FULL_NAME = "full-name";
static const char * const CONTACT_HASH_NICKNAME = "nickname";
static const char * const CONTACT_HASH_STRUCTURED_NAME = "structured-name";
static const char * const CONTACT_HASH_STRUCTURED_NAME_FAMILY = "family";
static const char * const CONTACT_HASH_STRUCTURED_NAME_GIVEN = "given";
static const char * const CONTACT_HASH_STRUCTURED_NAME_ADDITIONAL = "additional";
static const char * const CONTACT_HASH_STRUCTURED_NAME_PREFIXES = "prefixes";
static const char * const CONTACT_HASH_STRUCTURED_NAME_SUFFIXES = "suffixes";
static const char * const CONTACT_HASH_ALIAS = "alias";
static const char * const CONTACT_HASH_PHOTO = "photo";
static const char * const CONTACT_HASH_BIRTHDAY = "birthday";
static const char * const CONTACT_HASH_EMAILS = "emails";
static const char * const CONTACT_HASH_PHONES = "phones";
static const char * const CONTACT_HASH_URLS = "urls";
static const char * const CONTACT_HASH_NOTES = "notes";
static const char * const CONTACT_HASH_ADDRESSES = "addresses";
static const char * const CONTACT_HASH_ADDRESSES_PO_BOX = "po-box";
static const char * const CONTACT_HASH_ADDRESSES_EXTENSION = "extension";
static const char * const CONTACT_HASH_ADDRESSES_STREET = "street";
static const char * const CONTACT_HASH_ADDRESSES_LOCALITY = "locality";
static const char * const CONTACT_HASH_ADDRESSES_REGION = "region";
static const char * const CONTACT_HASH_ADDRESSES_POSTAL_CODE = "postal-code";
static const char * const CONTACT_HASH_ADDRESSES_COUNTRY = "country";
static const char * const CONTACT_HASH_ROLES = "roles";
static const char * const CONTACT_HASH_ROLES_ORGANISATION = "organisation";
static const char * const CONTACT_HASH_ROLES_TITLE = "title";
static const char * const CONTACT_HASH_ROLES_ROLE = "role";
static const char * const CONTACT_HASH_SOURCE = "source";

static const char * const INDIVIDUAL_DICT = "a{sv}";
static const char * const INDIVIDUAL_DICT_ENTRY = "{sv}";

template <class V> bool IsNonDefault(V value)
{
    // Default version uses normal C/C++ rules, for example pointer non-NULL.
    // For bool and integer, the value will only be sent if true or non-zero.
    return value;
}

template <> bool IsNonDefault<const gchar *>(const gchar *value)
{
    // Don't send empty strings.
    return value && value[0];
}

template <> bool IsNonDefault<GeeSet *>(GeeSet *value)
{
    // Don't send empty sets.
    return value && gee_collection_get_size(GEE_COLLECTION(value));
}

/**
 * Adds a dict entry to the builder, with 'key' as string key and the
 * result of 'get()' as value.
 */
template <class O, class V> void SerializeFolks(GDBusCXX::builder_type &builder,
                                                O *obj,
                                                V (*get)(O *),
                                                const char *key)
{
    if (!obj) {
        SE_THROW("casting to base class failed");
    }
    V value = get(obj);

    if (IsNonDefault(value)) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT_ENTRY)); // dict entry
        GDBusCXX::dbus_traits<std::string>::append(builder, key);
        g_variant_builder_open(&builder, G_VARIANT_TYPE("v")); // variant
        GDBusCXX::dbus_traits<V>::append(builder, value);
        g_variant_builder_close(&builder); // variant
        g_variant_builder_close(&builder); // dict entry
    }
}

template <class O, class V, class B> void SerializeFolks(GDBusCXX::builder_type &builder,
                                                         O *obj,
                                                         V (*get)(O *),
                                                         B *, // dummy parameter, determines base type
                                                         const char *key)
{
    if (!obj) {
        SE_THROW("casting to base class failed");
    }
    V value = get(obj);

    if (IsNonDefault(value)) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT_ENTRY)); // dict entry
        GDBusCXX::dbus_traits<std::string>::append(builder, key);
        g_variant_builder_open(&builder, G_VARIANT_TYPE("v")); // variant
        GDBusCXX::dbus_traits<B>::append(builder, value);
        g_variant_builder_close(&builder); // variant
        g_variant_builder_close(&builder); // dict entry
    }
}

SE_END_CXX

namespace GDBusCXX {

template <> struct dbus_traits<FolksStructuredName *> {
    static void append(builder_type &builder, FolksStructuredName *value) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT)); // dict
        SyncEvo::SerializeFolks(builder, value, folks_structured_name_get_family_name, CONTACT_HASH_STRUCTURED_NAME_FAMILY);
        SyncEvo::SerializeFolks(builder, value, folks_structured_name_get_given_name, CONTACT_HASH_STRUCTURED_NAME_GIVEN);
        SyncEvo::SerializeFolks(builder, value, folks_structured_name_get_additional_names, CONTACT_HASH_STRUCTURED_NAME_ADDITIONAL);
        SyncEvo::SerializeFolks(builder, value, folks_structured_name_get_prefixes, CONTACT_HASH_STRUCTURED_NAME_PREFIXES);
        SyncEvo::SerializeFolks(builder, value, folks_structured_name_get_suffixes, CONTACT_HASH_STRUCTURED_NAME_SUFFIXES);
        g_variant_builder_close(&builder); // dict
    }
};

template <> struct dbus_traits<GLoadableIcon *> {
    static void append(builder_type &builder, GLoadableIcon *value) {
        if (G_IS_FILE_ICON(value)) {
            GFileIcon *fileIcon = G_FILE_ICON(value);
            GFile *file = g_file_icon_get_file(fileIcon);
            if (file) {
                PlainGStr uri(g_file_get_uri(file));
                GDBusCXX::dbus_traits<const char *>::append(builder, uri);
                return;
            }
        }
        // EDS is expected to only work with URIs for the PHOTO
        // property, therefore we shouldn't get here. If we do, we
        // need to store something.
        GDBusCXX::dbus_traits<const char *>::append(builder, "");
    }
};

template <> struct dbus_traits<FolksPostalAddress *> {
    static void append(builder_type &builder, FolksPostalAddress *value) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT)); // dict
        SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_po_box, CONTACT_HASH_ADDRESSES_PO_BOX);
        SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_extension, CONTACT_HASH_ADDRESSES_EXTENSION);
        SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_street, CONTACT_HASH_ADDRESSES_STREET);
        SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_locality, CONTACT_HASH_ADDRESSES_LOCALITY);
        SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_region, CONTACT_HASH_ADDRESSES_REGION);
        SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_postal_code, CONTACT_HASH_ADDRESSES_POSTAL_CODE);
        SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_country, CONTACT_HASH_ADDRESSES_COUNTRY);

        // Not used by EDS. The tracker backend in folks was able to provide such a uid.
        // SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_address_format, CONTACT_HASH_ADDRESSES_FORMAT);
        // SyncEvo::SerializeFolks(builder, value, folks_postal_address_get_uid, "uid");
        g_variant_builder_close(&builder); // dict
    }
};

template <> struct dbus_traits<FolksRole *> {
    static void append(builder_type &builder, FolksRole *value) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT)); // dict
        // Other parts of ORG are not currently supported by libfolks.
        SyncEvo::SerializeFolks(builder, value, folks_role_get_organisation_name, CONTACT_HASH_ROLES_ORGANISATION);
        SyncEvo::SerializeFolks(builder, value, folks_role_get_title, CONTACT_HASH_ROLES_TITLE);
        SyncEvo::SerializeFolks(builder, value, folks_role_get_role, CONTACT_HASH_ROLES_ROLE);
        // Not used:
        // SyncEvo::SerializeFolks(builder, value, folks_role_get_uid, "uid");
        g_variant_builder_close(&builder); // dict
    }
};

// TODO: put into global header file
static const char * const MANAGER_PREFIX = "pim-manager-";

template <> struct dbus_traits<FolksPersona *> {
    static void append(builder_type &builder, FolksPersona *value) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE("(ss)")); // pair of peer ID and local ID
        const gchar *uid = folks_persona_get_uid(value);
        if (uid) {
            gchar *backend, *storeID, *personaID;
            folks_persona_split_uid(uid, &backend, &storeID, &personaID);
            PlainGStr tmp1(backend), tmp2(storeID), tmp3(personaID);
            if (boost::starts_with(storeID, MANAGER_PREFIX)) {
                dbus_traits<const char *>::append(builder, storeID + strlen(MANAGER_PREFIX));
            } else {
                // Must be the system address book.
                dbus_traits<const char *>::append(builder, "");
            }
            dbus_traits<const char *>::append(builder, personaID);
        } else {
            dbus_traits<const char *>::append(builder, "");
            dbus_traits<const char *>::append(builder, "");
        }
        g_variant_builder_close(&builder); // pair
    }
};

// Only use this with FolksAbstractFieldDetails instances where
// the value is a string.
template <> struct dbus_traits<FolksAbstractFieldDetails *> {
    static void append(builder_type &builder, FolksAbstractFieldDetails *value) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE("(sas)")); // pair of string and string list
        gconstpointer v = folks_abstract_field_details_get_value(value);
        dbus_traits<const char *>::append(builder, v ? (const char *)v : "");
        g_variant_builder_open(&builder, G_VARIANT_TYPE("as"));
        GeeMultiMap *map = folks_abstract_field_details_get_parameters(value);
        if (map) {
            BOOST_FOREACH (const char *type, GeeCollCXX<const char *>(gee_multi_map_get(map, FOLKS_ABSTRACT_FIELD_DETAILS_PARAM_TYPE))) {
                dbus_traits<const char *>::append(builder, type);
            }
        }
        g_variant_builder_close(&builder); // string list
        g_variant_builder_close(&builder); // pair
    }
};

template <> struct dbus_traits<FolksPostalAddressFieldDetails *> {
    static void append(builder_type &builder, FolksPostalAddressFieldDetails *value) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE(StringPrintf("(%sas)", INDIVIDUAL_DICT).c_str())); // pair of dict and string list
        FolksAbstractFieldDetails *fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS(value);
        gconstpointer v = folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(fieldDetails));
        dbus_traits<FolksPostalAddress *>::append(builder, static_cast<FolksPostalAddress *>(const_cast<gpointer>(v)));
        g_variant_builder_open(&builder, G_VARIANT_TYPE("as"));
        GeeMultiMap *map = folks_abstract_field_details_get_parameters(fieldDetails);
        if (map) {
            BOOST_FOREACH (const char *type, GeeCollCXX<const char *>(gee_multi_map_get(map, "type"))) {
                dbus_traits<const char *>::append(builder, type);
            }
        }
        g_variant_builder_close(&builder); // string list
        g_variant_builder_close(&builder); // pair
    }
};

template <> struct dbus_traits<FolksNoteFieldDetails *> {
    static void append(builder_type &builder, FolksNoteFieldDetails *value) {
        FolksAbstractFieldDetails *fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS(value);
        gconstpointer v = folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(fieldDetails));
        dbus_traits<const char *>::append(builder, static_cast<const char *>(v)); // plain string
        // Ignore parameters. LANGUAGE is hardly ever set.
    }
};

template <> struct dbus_traits<FolksRoleFieldDetails *> {
    static void append(builder_type &builder, FolksRoleFieldDetails *value) {
        FolksAbstractFieldDetails *fieldDetails = FOLKS_ABSTRACT_FIELD_DETAILS(value);
        gconstpointer v = folks_abstract_field_details_get_value(FOLKS_ABSTRACT_FIELD_DETAILS(fieldDetails));
        dbus_traits<FolksRole *>::append(builder, static_cast<FolksRole *>(const_cast<gpointer>((v))));
        // Ignore parameters. LANGUAGE is hardly ever set.
    }
};

// Used for types like V = FolksEmailFieldDetails *
template <class V> struct dbus_traits< GeeCollCXX<V> > {
    static void append(builder_type &builder, const GeeCollCXX<V> &collection) {
        g_variant_builder_open(&builder, G_VARIANT_TYPE("av")); // array of variants
        BOOST_FOREACH (V value, collection) {
            g_variant_builder_open(&builder, G_VARIANT_TYPE("v")); // variant
            dbus_traits<V>::append(builder, value);
            g_variant_builder_close(&builder); // variant
        }
        g_variant_builder_close(&builder); // array of variants
    }
};

template <> struct dbus_traits<GDateTime *> {
    static void append(builder_type &builder, GDateTime *value) {
        // Extract local date from UTC date + time + UTC offset.
        //
        // The libfolks EDS backend does date + 00:00 in local time,
        // then converts to UTC. We need to hard-code the stripping
        // of the time. Folks should make it easier to extract the date, see
        // https://bugzilla.gnome.org/show_bug.cgi?id=684905
        //
        // TODO: if folks doesn't get fixed, then we should cache the
        // GTimeZone local = g_time_zone_new_local()
        // and use that throughout the runtime of the process, like
        // folks-eds does.
        GDateTimeCXX local(g_date_time_to_local(value), false);
        gint year, month, day;
        g_date_time_get_ymd(local.get(), &year, &month, &day);
        g_variant_builder_open(&builder, G_VARIANT_TYPE("(iii)")); // tuple with year, month, day
        GDBusCXX::dbus_traits<int>::append(builder, year);
        GDBusCXX::dbus_traits<int>::append(builder, month);
        GDBusCXX::dbus_traits<int>::append(builder, day);
        g_variant_builder_close(&builder); // tuple
    }
};


} // namespace GDBusCXX

SE_BEGIN_CXX

void DBus2FolksIndividual(GDBusCXX::reader_type &iter, FolksIndividualCXX &individual)
{
    individual = FolksIndividualCXX::steal(folks_individual_new(NULL));
}

void FolksIndividual2DBus(const FolksIndividualCXX &individual, GDBusCXX::builder_type &builder)
{
    g_variant_builder_open(&builder, G_VARIANT_TYPE(INDIVIDUAL_DICT)); // dict

    FolksNameDetails *name = FOLKS_NAME_DETAILS(individual.get());
    SerializeFolks(builder, name, folks_name_details_get_full_name,
                   CONTACT_HASH_FULL_NAME);
    SerializeFolks(builder, name, folks_name_details_get_nickname,
                   CONTACT_HASH_NICKNAME);
    SerializeFolks(builder, name, folks_name_details_get_structured_name,
                   CONTACT_HASH_STRUCTURED_NAME);

    // gconstpointer folks_abstract_field_details_get_value (FolksAbstractFieldDetails* self);

    FolksAliasDetails *alias = FOLKS_ALIAS_DETAILS(individual.get());
    SerializeFolks(builder, alias, folks_alias_details_get_alias,
                   CONTACT_HASH_ALIAS);

    FolksAvatarDetails *avatar = FOLKS_AVATAR_DETAILS(individual.get());
    SerializeFolks(builder, avatar, folks_avatar_details_get_avatar,
                   CONTACT_HASH_PHOTO);

    FolksBirthdayDetails *birthday = FOLKS_BIRTHDAY_DETAILS(individual.get());
    SerializeFolks(builder, birthday, folks_birthday_details_get_birthday,
                   CONTACT_HASH_BIRTHDAY);
    // const gchar* folks_birthday_details_get_calendar_event_id (FolksBirthdayDetails* self);

    FolksEmailDetails *emails = FOLKS_EMAIL_DETAILS(individual.get());
    SerializeFolks(builder, emails, folks_email_details_get_email_addresses,
                   (GeeCollCXX<FolksAbstractFieldDetails *>*)NULL,
                   CONTACT_HASH_EMAILS);

    FolksPhoneDetails *phones = FOLKS_PHONE_DETAILS(individual.get());
    SerializeFolks(builder, phones, folks_phone_details_get_phone_numbers,
                   (GeeCollCXX<FolksAbstractFieldDetails *>*)NULL,
                   CONTACT_HASH_PHONES);

    FolksUrlDetails *urls = FOLKS_URL_DETAILS(individual.get());
    SerializeFolks(builder, urls, folks_url_details_get_urls,
                   (GeeCollCXX<FolksAbstractFieldDetails *>*)NULL,
                   CONTACT_HASH_URLS);

    // Doesn't work like this, folks_im_details_get_im_addresses returns
    // a GeeMultiMap, not a GeeList. Not required anyway.
    // FolksImDetails *im = FOLKS_IM_DETAILS(individual.get());
    // SerializeFolks(builder, im, folks_im_details_get_im_addresses,
    //                (GeeCollCXX<FolksAbstractFieldDetails *>*)NULL, "im");

    FolksNoteDetails *notes = FOLKS_NOTE_DETAILS(individual.get());
    SerializeFolks(builder, notes, folks_note_details_get_notes,
                   (GeeCollCXX<FolksNoteFieldDetails *>*)NULL,
                   CONTACT_HASH_NOTES);

    FolksPostalAddressDetails *postal = FOLKS_POSTAL_ADDRESS_DETAILS(individual.get());
    SerializeFolks(builder, postal, folks_postal_address_details_get_postal_addresses,
                   (GeeCollCXX<FolksPostalAddressFieldDetails *>*)NULL,
                   CONTACT_HASH_ADDRESSES);

    FolksRoleDetails *roles = FOLKS_ROLE_DETAILS(individual.get());
    SerializeFolks(builder, roles, folks_role_details_get_roles,
                   (GeeCollCXX<FolksRoleFieldDetails *>*)NULL,
                   CONTACT_HASH_ROLES);

    SerializeFolks(builder, individual.get(), folks_individual_get_personas,
                   (GeeCollCXX<FolksPersona *>*)NULL,
                   CONTACT_HASH_SOURCE);

#if 0
    // Not exposed via D-Bus.
FolksGender folks_gender_details_get_gender (FolksGenderDetails* self);
GeeSet* folks_group_details_get_groups (FolksGroupDetails* self);
GeeMultiMap* folks_web_service_details_get_web_service_addresses (FolksWebServiceDetails* self);
guint folks_interaction_details_get_im_interaction_count (FolksInteractionDetails* self);
GDateTime* folks_interaction_details_get_last_im_interaction_datetime (FolksInteractionDetails* self);
guint folks_interaction_details_get_call_interaction_count (FolksInteractionDetails* self);
GDateTime* folks_interaction_details_get_last_call_interaction_datetime (FolksInteractionDetails* self);
GeeSet* folks_local_id_details_get_local_ids (FolksLocalIdDetails* self);
const gchar* folks_presence_details_get_default_message_from_type (FolksPresenceType type);
FolksPresenceType folks_presence_details_get_presence_type (FolksPresenceDetails* self);
const gchar* folks_presence_details_get_presence_message (FolksPresenceDetails* self);
const gchar* folks_presence_details_get_presence_status (FolksPresenceDetails* self);
#endif

    g_variant_builder_close(&builder); // dict
}

SE_END_CXX
