/*
 * Copyright (C) 2016 Emanoil Kotsev emanoil.kotsev@fincom.at
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
 *
 * $Id: TDEPIMAddressBookSourceRegister.cpp,v 1.4 2016/09/08 22:58:08 emanoil Exp $
 *
 */

SE_BEGIN_CXX

static SyncSource *createSource(const SyncSourceParams &params)
{
	SourceType sourceType = SyncSource::getSourceType(params.m_nodes);
	
	bool isMe = sourceType.m_backend == "TDE PIM Address Book";

#ifndef ENABLE_TDEPIMABC
	if (isMe) return RegisterSyncSource::InactiveSource(params);
#else
	if (isMe || sourceType.m_backend == "addressbook" ) {
		if (sourceType.m_format == "" || sourceType.m_format == "text/vcard") {
			return new TDEPIMAddressBookSource(TDEPIM_CONTACT_V30, params);
			}
		else if (sourceType.m_format == "text/x-vcard") {
			return new TDEPIMAddressBookSource(TDEPIM_CONTACT_V21, params);
			}
		else return NULL;
	}
#endif
	return NULL;
}

static class RegisterTDEPIMAddressBokSyncSource : public RegisterSyncSource
{
public:
    RegisterTDEPIMAddressBokSyncSource() :
	RegisterSyncSource ("TDE PIM Address Book/Contacts",
#ifdef ENABLE_TDEPIMABC
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "TDE PIM Address Book = TDE PIM Contacts = tdepim-contacts\n"
                                     "   vCard 2.1 = text/x-vcard\n"
                                     "   vCard 3.0 (default) = text/vcard\n"
                                     "   The later is the internal format of TDE PIM and preferred with\n"
                                     "   servers that support it.",
                                     Values() +
                                     (Aliases("TDE PIM Address Book") + "TDE PIM Contacts" + "tdepim-contacts")
			)
    {
        // configure and register our own property;
        // do this regardless whether the backend is enabled,
        // so that config migration always includes this property
/*        WebDAVCredentialsOkay().setHidden(true);
        SyncConfig::getRegistry().push_back(&WebDAVCredentialsOkay());
*/
    }
} registerMe;

SE_END_CXX
