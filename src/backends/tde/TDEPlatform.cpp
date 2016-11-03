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
 *
 * $Id: TDEPlatform.cpp,v 1.5 2016/09/12 19:57:42 emanoil Exp $
 *
 */

/*
*
* WARNING This code is untested! It is based on theory. Feedback is welcome!
*
*/

#include <config.h>

#ifdef ENABLE_TDEWALLET

#include "TDEPlatform.h"

#include <syncevo/Exception.h>
#include <syncevo/UserInterface.h>
#include <syncevo/SyncConfig.h>


#include <tdewallet.h>
#include <dcopclient.h>
#include <tdeapplication.h>
#include <tdeaboutdata.h>
#include <tdecmdlineargs.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

// TODO: this check should be global
// static bool HaveDBus;

void TDEInitMainSlot(const char *appname)
{

	//connect to dcop
	DCOPClient *kn_dcop = TDEApplication::kApplication()->dcopClient();
	if (!kn_dcop)
		Exception::throwError(SE_HERE, "internal init error, unable to make new dcop instance for tdenotes");

	TQString appId = kn_dcop->registerAs("syncevolution-tdewallet");

/*	SyncSourceLogging::init(InitList<std::string>("SUMMARY") + "LOCATION",
				" ",
				m_operations);
*/
	
}

static bool UseTDEWallet(const InitStateTri &keyring,
                       int slotCount)
{
	// Disabled by user?
	if (keyring.getValue() == InitStateTri::VALUE_FALSE) {
		return false;
	}
	
	// When both (presumably) GNOME keyring and TDE Wallet are available,
	// check if the user really wanted TDE Wallet before using TDE Wallet
	// instead of GNOME keyring. This default favors GNOME keyring
	// over TDE Wallet because SyncEvolution traditionally used that.
	if (keyring.getValue() == InitStateTri::VALUE_TRUE &&
		slotCount > 1) {
		return false;
	}
	
	// If explicitly selected, it must be us.
	if (keyring.getValue() == InitStateTri::VALUE_STRING &&
		!boost::iequals(keyring.get(), "TDE")) {
		return false;
	}
	
	// Use KWallet.
	return true;
}

/**
 * Here we use server sync url without protocol prefix and
 * user account name as the key in the keyring.
 *
 * Also since the KWallet's API supports only storing (key,password)
 * or Map<TQString,TQString> , the former is used.
 */
bool TDEWalletLoadPasswordSlot(const InitStateTri &keyring,
			const std::string &passwordName,
			const std::string &descr,
			const ConfigPasswordKey &key,
			InitStateString &password)
{
	if (!UseTDEWallet(keyring,
			GetLoadPasswordSignal().num_slots() - INTERNAL_LOAD_PASSWORD_SLOTS)) {
		SE_LOG_DEBUG(NULL, "not using TDE Wallet");
		return false;
	}
	
	TQString walletPassword;
	TQString walletKey = TQString(key.user.c_str()) + ',' +
		TQString(key.domain.c_str())+ ','+
		TQString(key.server.c_str())+','+
		TQString(key.object.c_str())+','+
		TQString(key.protocol.c_str())+','+
		TQString(key.authtype.c_str())+','+
		TQString::number(key.port);
	
//	TQString wallet_name = TDEWallet::Wallet::NetworkWallet();
	TQString wallet_name = TDEWallet::Wallet::LocalWallet();
	
	const TQString folder("Syncevolution");
	
	bool found = false;
	if (!TDEWallet::Wallet::keyDoesNotExist(wallet_name, folder, walletKey)) {
		TDEWallet::Wallet *wallet = TDEWallet::Wallet::openWallet(wallet_name, -1, TDEWallet::Wallet::Synchronous);
		if ( wallet &&
			wallet->setFolder(folder) &&
			wallet->readPassword(walletKey, walletPassword) == 0 ) {
			std::string text1(walletPassword.utf8(),walletPassword.utf8().length());
			password = text1;
			found = true;
		}
	}
	SE_LOG_DEBUG(NULL, "%s password in KWallet using %s",
			found ? "found" : "no",
			key.toString().c_str());
	
	return true;
}
	
	
bool TDEWalletSavePasswordSlot(const InitStateTri &keyring,
			const std::string &passwordName,
			const std::string &password,
			const ConfigPasswordKey &key)
{
	if (!UseTDEWallet(keyring,
			GetSavePasswordSignal().num_slots() - INTERNAL_SAVE_PASSWORD_SLOTS)) {
		SE_LOG_DEBUG(NULL, "not using TDE Wallet");
		return false;
	}
	
	/*
	* It is possible to let CmdlineSyncClient decide which 
	* fields in ConfigPasswordKey it would use
	* but currently only use passed key instead 
	*/
	
	// write password to keyring
	std::string s = key.user + ',' + key.domain + ',' + key.server + ',' + key.object + ',' + key.protocol + ',' + key.authtype + ',';
	const TQString walletKey =TQString::fromUtf8(s.data(),s.size()) + TQString::number(key.port);
	const TQString walletPassword = TQString::fromUtf8(password.data(),password.size());
	
	bool write_success = false;
	const TQString wallet_name = TDEWallet::Wallet::NetworkWallet();
	const TQString folder ("Syncevolution");
	TDEWallet::Wallet *wallet = TDEWallet::Wallet::openWallet(wallet_name, -1,
								TDEWallet::Wallet::Synchronous);
	if (wallet) {
		if (!wallet->hasFolder(folder)) {
			wallet->createFolder(folder);
		}
	
		if (wallet->setFolder(folder) &&
		    wallet->writePassword(walletKey, walletPassword) == 0) {
			write_success = true;
		}
	}
	
	if (!write_success) {
		Exception::throwError(SE_HERE, "Saving " + passwordName + " in TDE Wallet failed.");
	}
	SE_LOG_DEBUG(NULL, "stored password in KWallet using %s", key.toString().c_str());
	return write_success;
}

SE_END_CXX

#endif // ENABLE_TDEWALLET
