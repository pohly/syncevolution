/**
 * Browser D-Bus Bridge, XPCOM version
 *
 * Copyright © 2008 Movial Creative Technologies Inc
 *  Contact: Movial Creative Technologies Inc, <info@movial.com>
 *  Authors: Lauri Mylläri, <lauri.myllari@movial.com>
 *           Kalle Vahlman, <kalle.vahlman@movial.com>
 *
 * The contents of this file are subject to the Mozilla Public License
 * Version 1.1 (the "License"); you may not use this file except in
 * compliance with the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS"
 * basis, WITHOUT WARRANTY OF ANY KIND, either express or implied. See the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * The Original Code is the Browser D-Bus Bridge, XPCOM version.
 *
 * The Initial Developer of the Original Code is Movial Creative Technologies
 * Inc. Portions created by Initial Developer are Copyright (C) 2008
 * Movial Creative Technologies Inc. All Rights Reserved.
 *
 */

#include <stdio.h>
#include <glib.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus-glib.h>


#include "nsIGenericFactory.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsComponentManagerUtils.h"
#include "nsServiceManagerUtils.h"

#include "nsEmbedString.h"
#include "nsIMutableArray.h"
#include "nsArrayUtils.h"
#include "nsIXPConnect.h"

#include "IDBusService.h"

#include "DBusService.h"
#include "DBusMethod.h"
#include "DBusSignal.h"
#include "DBusDataCarrier.h"
#include "DBusMarshaling.h"

#include "bdb-debug.h"

//
// DBusService implementation
//

static
DBusHandlerResult _signal_filter(DBusConnection *connection,
                                 DBusMessage *message,
                                 void *user_data);

static DBusService *gDBusService = nsnull;

NS_IMPL_ISUPPORTS1(DBusService, IDBusService);

DBusService::DBusService() :
    mSystemBus(nsnull),
    mSessionBus(nsnull),
    mSystemBusHasFilter(PR_FALSE),
    mSessionBusHasFilter(PR_FALSE),
    mInsideEmit(PR_FALSE)
{
    BDBLOG(("DBusService::DBusService()\n"));
    mSystemBusSignalObservers.Init();
    mSessionBusSignalObservers.Init();
}

DBusService::~DBusService()
{
    BDBLOG(("DBusService::~DBusService()\n"));
    /* FIXME - check if connections need to be released */
}

NS_IMETHODIMP
DBusService::GetSignal(PRUint32 aBusType,
                       const nsACString& aInterfaceName,
                       const nsACString& aSignalName,
                       const nsACString& aSender,
                       const nsACString& aObjectPath,
                       IDBusSignal **_retval)
{
    nsClassHashtable<nsCStringHashKey, nsTArray<nsWeakPtr> > *signalObservers = nsnull;
    *_retval = nsnull;

    GetConnection(aBusType);

    if (aBusType == SYSTEM)
    {
        if (!mSystemBusHasFilter)
        {
            signalObservers = &mSystemBusSignalObservers;
            mSystemBusHasFilter = PR_TRUE;
        }
    }
    else if (aBusType == SESSION)
    {
        if (!mSessionBusHasFilter)
        {
            signalObservers = &mSessionBusSignalObservers;
            mSessionBusHasFilter = PR_TRUE;
        }
    }
    else
    {
        BDBLOG(("DBusService::GetSignal(): unknown bus type %d\n", aBusType));
        return NS_ERROR_ILLEGAL_VALUE;
    }

    /* add filter only once for each connection */
    if (signalObservers)
        dbus_connection_add_filter(GetConnection(aBusType),
                                   _signal_filter,
                                   signalObservers,
                                   nsnull);

    IDBusSignal *signal = new DBusSignal(this,
                                         aBusType,
                                         aInterfaceName,
                                         aSignalName,
                                         aSender,
                                         aObjectPath,
                                         GetCurrentJSContext());

    NS_ENSURE_TRUE(signal, NS_ERROR_OUT_OF_MEMORY);

    NS_ADDREF(*_retval = signal);

    return NS_OK;
}

NS_IMETHODIMP
DBusService::GetMethod(PRUint32 aBusType,
                       const nsACString& aDestination,
                       const nsACString& aObjectPath,
                       const nsACString& aMethodName,
                       const nsACString& aInterfaceName,
                       const nsACString& aSignature,
                       IDBusMethod **_retval)
{
    *_retval = nsnull;

    if (!GetConnection(aBusType))
    {
        BDBLOG(("DBusService::GetMethod()): invalid bus type %d\n",
               aBusType));
        return NS_ERROR_ILLEGAL_VALUE;
    }

    if (!dbus_signature_validate(PromiseFlatCString(aSignature).get(), nsnull))
    {
        BDBLOG(("DBusService::GetMethod()): invalid method signature '%s'\n",
               PromiseFlatCString(aSignature).get()));
        return NS_ERROR_ILLEGAL_VALUE;
    }

    IDBusMethod *method = new DBusMethod(this,
                                         aBusType,
                                         aDestination,
                                         aObjectPath,
                                         aMethodName,
                                         aInterfaceName,
                                         aSignature,
                                         GetCurrentJSContext());

    NS_ENSURE_TRUE(method, NS_ERROR_OUT_OF_MEMORY);

    NS_ADDREF(*_retval = method);

    return NS_OK;
}

NS_IMETHODIMP DBusService::EmitSignal(PRUint32 busType,
                                      const nsACString & objectPath,
                                      const nsACString & interfaceName,
                                      const nsACString & signalName,
                                      const nsACString & aSignature,
                                      nsIVariant **args,
                                      PRUint32 count,
                                      PRBool *_retval)
{
    DBusMessage *msg;
    DBusMessageIter msg_iter;
    DBusConnection *conn;
    nsCAutoString signature;

    conn = GetConnection(busType);

    if (!conn)
    {
        BDBLOG(("DBusService::EmitSignal()): invalid bus type %d\n",
               busType));
        return NS_ERROR_ILLEGAL_VALUE;
    }

    if (objectPath.IsEmpty()
     || interfaceName.IsEmpty()
     || signalName.IsEmpty())
    {
        BDBLOG(("DBusService::EmitSignal()): invalid signal arguments\n"));
        return NS_ERROR_ILLEGAL_VALUE;
    }

    msg = dbus_message_new_signal(PromiseFlatCString(objectPath).get(),
                                  PromiseFlatCString(interfaceName).get(),
                                  PromiseFlatCString(signalName).get());
    dbus_message_iter_init_append(msg, &msg_iter);

    if (count > 0)
    {
        JSContext *cx = GetCurrentJSContext();

        if (aSignature.Equals(""))
        {
            for (PRUint32 i = 0; i < count; i++)
            {
                // no method signature specified, guess argument types
                nsCOMPtr<nsIVariant> data = args[i];
                nsCAutoString tmpsig;

                getSignatureFromVariant(cx, data, tmpsig);
                BDBLOG(("  aArgs[%02d]       : signature \"%s\"\n",
                       i,
                       PromiseFlatCString(tmpsig).get()));
                signature.Append(tmpsig);

            }
        } else {
            signature.Assign(aSignature);
        }

        if (dbus_signature_validate(PromiseFlatCString(signature).get(), nsnull))
        {
            DBusSignatureIter sig_iter;
            int current_type;
            int i = 0;

            BDBLOG(("  signature \"%s\"\n", PromiseFlatCString(signature).get()));

            dbus_signature_iter_init(&sig_iter, PromiseFlatCString(signature).get());
            while ((current_type = dbus_signature_iter_get_current_type(&sig_iter)) != DBUS_TYPE_INVALID)
            {
                char *element_signature = dbus_signature_iter_get_signature(&sig_iter);
                BDBLOG(("  element \"%s\" from signature\n", element_signature));
                BDBLOG(("  type %c from signature\n", current_type));

                addVariantToIter(cx, args[i], &msg_iter, &sig_iter);

                i++;
                dbus_free(element_signature);
                dbus_signature_iter_next(&sig_iter);
            }
        }
        else
        {
            BDBLOG(("  invalid signature \"%s\"\n", PromiseFlatCString(signature).get()));
            dbus_message_unref(msg);
            return NS_ERROR_ILLEGAL_VALUE;
        }
    }

    if (dbus_connection_send(conn, msg, NULL))
    {
        dbus_message_unref(msg);
        return NS_OK;
    }
        
    dbus_message_unref(msg);
    return NS_ERROR_UNEXPECTED;
}
 

DBusPendingCall *DBusService::SendWithReply(PRUint32 aConnType,
                                            DBusMessage *aMessage,
                                            PRUint32 aTimeout)
{
    DBusPendingCall *retval = nsnull;
    DBusConnection *conn = GetConnection(aConnType);

    if (!conn)
        return nsnull;

    if (!dbus_connection_send_with_reply(conn,
                                         aMessage,
                                         &retval,
                                         aTimeout))
        return nsnull;

    return retval;
}

DBusMessage *DBusService::SendWithReplyAndBlock(PRUint32 aConnType,
                                                DBusMessage *aMessage,
                                                PRUint32 aTimeout,
                                                DBusError *aError)
{
    DBusConnection *conn = GetConnection(aConnType);

    if (!conn)
        return nsnull;

    return dbus_connection_send_with_reply_and_block(conn,
                                                     aMessage,
                                                     aTimeout,
                                                     aError);
}

static
DBusHandlerResult _signal_filter(DBusConnection *connection,
                                 DBusMessage *message,
                                 void *user_data)
{
    if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_SIGNAL)
    {
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    nsClassHashtable<nsCStringHashKey, nsTArray<nsWeakPtr> > *observerHash =
        (nsClassHashtable<nsCStringHashKey, nsTArray<nsWeakPtr> > *) user_data;

    BDBLOG(("_signal_filter: %s.%s\n",
           dbus_message_get_interface(message),
           dbus_message_get_member(message)));

    nsCAutoString observerKey;

    observerKey.Assign(dbus_message_get_interface(message));
    observerKey.Append(NS_LITERAL_CSTRING("."));
    observerKey.Append(dbus_message_get_member(message));

    BDBLOG(("  observerKey: '%s'\n",
           PromiseFlatCString(observerKey).get()));

    nsTArray<nsWeakPtr> *observerList = nsnull;

    observerHash->Get(observerKey, &observerList);
    if (observerList)
    {
        BDBLOG(("  got observerList\n"));
        DBusService *service = DBusService::GetSingleton();
        
        for (PRUint32 i = 0; i < observerList->Length(); ++i) {
            nsCAutoString t;
            nsCOMPtr<IDBusSignal> signal = do_QueryReferent((*observerList)[i]);
            signal->GetInterfaceName(t);
            BDBLOG(("    interface : %s\n", PromiseFlatCString(t).get()));
            signal->GetSignalName(t);
            BDBLOG(("    signal    : %s\n", PromiseFlatCString(t).get()));
            signal->GetSender(t);
            BDBLOG(("    sender    : %s\n", PromiseFlatCString(t).get()));
            if (!t.IsEmpty() && !t.Equals(dbus_message_get_sender(message)))
            {
                BDBLOG(("    sender does not match\n"));
                continue;
            }
            signal->GetObjectPath(t);
            BDBLOG(("    object    : %s\n", PromiseFlatCString(t).get()));
            if (!t.IsEmpty() && !t.Equals(dbus_message_get_path(message)))
            {
                BDBLOG(("    objectPath does not match\n"));
                continue;
            }

            /* do callback */

            DBusMessageIter iter;
            nsCOMPtr<nsIMutableArray> args_array;

            dbus_message_iter_init(message, &iter);
            JSContext *cx;
            signal->GetJSContext(&cx);
            args_array = getArrayFromIter(cx, &iter);

            PRUint32 arg_items;
            args_array->GetLength(&arg_items);
            BDBLOG(("  arg_items: %d items\n", arg_items));

            /* arguments are packed as an array into an nsIVariant */
            nsIVariant **callback_args = new nsIVariant*[arg_items];
            nsCOMPtr<nsIWritableVariant> args = do_CreateInstance("@mozilla.org/variant;1");
            for (PRUint32 i = 0; i < arg_items; i++)
            {
                nsCOMPtr<nsIVariant> arg = do_QueryElementAt(args_array, i);
                callback_args[i] = arg;
                NS_ADDREF(callback_args[i]);
            }
            args->SetAsArray(nsIDataType::VTYPE_INTERFACE_IS,
                             &NS_GET_IID(nsIVariant),
                             arg_items,
                             callback_args);
            for (PRUint32 i = 0; i < arg_items; i++)
                NS_RELEASE(callback_args[i]);
            delete[] callback_args;

            nsCOMPtr<IDBusSignalObserver> callback;
            signal->GetOnEmit(getter_AddRefs(callback));

            service->SetInsideEmit(TRUE);
            callback->OnSignal(args);
            service->SetInsideEmit(FALSE);

        }
        
        /* Check if we have queued observer changes */
        service->CheckSignalObserverQueue();
        
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    else
    {
        BDBLOG(("  no observer found\n"));
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }
}

static
void BuildRule(IDBusSignal *aSignal, nsACString& aRetval)
{
    nsCAutoString tmp;

    aRetval.Assign(NS_LITERAL_CSTRING("type='signal',interface='"));
    aSignal->GetInterfaceName(tmp);
    aRetval.Append(tmp);
    aRetval.Append(NS_LITERAL_CSTRING("',member='"));
    aSignal->GetSignalName(tmp);
    aRetval.Append(tmp);
    aRetval.Append(NS_LITERAL_CSTRING("'"));
}

void DBusService::CheckSignalObserverQueue()
{
    BDBLOG(("%s\n", __FUNCTION__));

    PRUint32 i = mRemovedSignals.Length();
    while (i-- > 0)
    {
        RemoveSignalObserver(mRemovedSignals[i]);
        mRemovedSignals.RemoveElementAt(i);
    }
    i = mAddedSignals.Length();
    while (i-- > 0)
    {
        AddSignalObserver(mAddedSignals[i]);
        mAddedSignals.RemoveElementAt(i);
    }
}

void DBusService::AddSignalObserver(IDBusSignal *aSignal)
{
    nsClassHashtable<nsCStringHashKey, nsTArray<nsWeakPtr> > *signalObservers = nsnull;
    nsCAutoString observerKey;
    nsCAutoString tmp;

    if (mInsideEmit)
    {
        mAddedSignals.AppendElement(aSignal);
        return;
    }

    BDBLOG(("DBusService::AddSignalObserver()\n"));

    aSignal->GetInterfaceName(tmp);
    BDBLOG(("  aInterface : %s\n", PromiseFlatCString(tmp).get()));
    observerKey.Assign(tmp);
    observerKey.Append(".");

    aSignal->GetSignalName(tmp);
    BDBLOG(("  aSignal    : %s\n", PromiseFlatCString(tmp).get()));
    observerKey.Append(tmp);

    BDBLOG(("  observerKey: %s\n", PromiseFlatCString(observerKey).get()));

    nsTArray<nsWeakPtr> *observerList = nsnull;

    PRUint32 bus_type = 0;
    aSignal->GetBusType(&bus_type);
    if (bus_type == SYSTEM)
        signalObservers = &mSystemBusSignalObservers;
    else if (bus_type == SESSION)
        signalObservers = &mSessionBusSignalObservers;
    signalObservers->Get(observerKey, &observerList);
    if (observerList)
    {
        /* append to list */
        BDBLOG(("  got observerList\n"));
        nsCOMPtr<nsISupportsWeakReference> weakRefable = do_QueryInterface(aSignal);
        nsWeakPtr weakPtr = getter_AddRefs(NS_GetWeakReference(weakRefable));
        observerList->AppendElement(weakPtr);
    }
    else
    {
        /* create a new list */
        BDBLOG(("  no observerList found\n"));
        observerList = new nsTArray<nsWeakPtr>;
        nsCOMPtr<nsISupportsWeakReference> weakRefable = do_QueryInterface(aSignal);
        nsWeakPtr weakPtr = getter_AddRefs(NS_GetWeakReference(weakRefable));
        observerList->AppendElement(weakPtr);
        signalObservers->Put(observerKey, observerList);
        
        /* add match rule for interface.signal */
        PRUint32 busType;
        nsCAutoString matchRule;

        aSignal->GetBusType(&busType);
        BuildRule(aSignal, matchRule);
        BDBLOG(("  new match rule: %s\n", PromiseFlatCString(matchRule).get()));
        dbus_bus_add_match(GetConnection(busType),
                           PromiseFlatCString(matchRule).get(),
                           nsnull);
    }
}

void DBusService::RemoveSignalObserver(IDBusSignal *aSignal)
{
    nsClassHashtable<nsCStringHashKey, nsTArray<nsWeakPtr> > *signalObservers = nsnull;
    nsCAutoString observerKey;
    nsCAutoString tmp;

    if (mInsideEmit)
    {
        mRemovedSignals.AppendElement(aSignal);
        return;
    }

    BDBLOG(("DBusService::RemoveSignalObserver()\n"));

    aSignal->GetInterfaceName(tmp);
    BDBLOG(("  aInterface : %s\n", PromiseFlatCString(tmp).get()));
    observerKey.Assign(tmp);
    observerKey.Append(".");

    aSignal->GetSignalName(tmp);
    BDBLOG(("  aSignal    : %s\n", PromiseFlatCString(tmp).get()));
    observerKey.Append(tmp);

    BDBLOG(("  observerKey: %s\n", PromiseFlatCString(observerKey).get()));

    nsTArray<nsWeakPtr> *observerList = nsnull;

    PRUint32 bus_type = 0;
    aSignal->GetBusType(&bus_type);
    if (bus_type == SYSTEM)
        signalObservers = &mSystemBusSignalObservers;
    else if (bus_type == SESSION)
        signalObservers = &mSessionBusSignalObservers;
    signalObservers->Get(observerKey, &observerList);
    if (observerList)
    {
        BDBLOG(("  got observerList\n"));
        nsCOMPtr<nsISupportsWeakReference> weakRefable = do_QueryInterface(aSignal);
        nsWeakPtr weakPtr = getter_AddRefs(NS_GetWeakReference(weakRefable));
        for (PRUint32 i = 0; i < observerList->Length(); ++i) {
            nsCAutoString t;
            nsCAutoString ob;
            nsCOMPtr<IDBusSignal> signal = do_QueryReferent((*observerList)[i]);
            signal->GetInterfaceName(t);
            ob.Assign(t);
            ob.Append(".");
            signal->GetSignalName(t);
            ob.Append(t);
            BDBLOG(("    signal : %s\n", PromiseFlatCString(ob).get()));
        }
        BDBLOG(("  call observerList->RemoveElement\n"));
        observerList->RemoveElement(weakPtr);
        for (PRUint32 i = 0; i < observerList->Length(); ++i) {
            nsCAutoString t;
            nsCAutoString ob;
            nsCOMPtr<IDBusSignal> signal = do_QueryReferent((*observerList)[i]);
            signal->GetInterfaceName(t);
            ob.Assign(t);
            ob.Append(".");
            signal->GetSignalName(t);
            ob.Append(t);
            BDBLOG(("    signal : %s\n", PromiseFlatCString(ob).get()));
        }

        // if list is empty, remove match rule
        if (observerList->Length() == 0)
        {
            PRUint32 busType;
            nsCAutoString matchRule;

            aSignal->GetBusType(&busType);
            BuildRule(aSignal, matchRule);
            BDBLOG(("  remove match rule: %s\n", PromiseFlatCString(matchRule).get()));
            dbus_bus_remove_match(GetConnection(busType),
                                  PromiseFlatCString(matchRule).get(),
                                  nsnull);
            signalObservers->Remove(observerKey); 
        }
        BDBLOG(("  done\n"));
    }
    else
    {
        BDBLOG(("  ERROR: no observerList found!\n"));
    }
}

JSContext *DBusService::GetCurrentJSContext()
{
    // try to get a JS context (code borrowed from xpcsample1.cpp)

    // get the xpconnect service
    nsresult rv;
    nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
    if(NS_FAILED(rv))
        return nsnull;
    BDBLOG(("    got nsIXPConnect\n"));

    // get the xpconnect native call context
    nsAXPCNativeCallContext *callContext = nsnull;
    xpc->GetCurrentNativeCallContext(&callContext);
    if(!callContext)
    {
    BDBLOG(("    callContext :(\n"));
        return nsnull;
    }
    // Get JSContext of current call
    JSContext* cx;
    rv = callContext->GetJSContext(&cx);
    if(NS_FAILED(rv) || !cx)
        return nsnull;
    BDBLOG(("    got JSContext\n"));

    return cx;
}

DBusConnection *DBusService::GetConnection(PRUint32 aConnType)
{
    BDBLOG(("DBusService::GetConnection(%d)\n", aConnType));

    if (aConnType == SYSTEM)
    {
        if (mSystemBus == nsnull)
        {
            mSystemBus = dbus_bus_get(DBUS_BUS_SYSTEM, NULL);
            if (!mSystemBus)
                return nsnull;
            dbus_connection_set_exit_on_disconnect(mSystemBus, PR_FALSE);
            dbus_connection_setup_with_g_main(mSystemBus, NULL);
        }
        return mSystemBus;
    }
    else if (aConnType == SESSION)
    {
        if (mSessionBus == nsnull)
        {
            mSessionBus = dbus_bus_get(DBUS_BUS_SESSION, NULL);
            if (!mSessionBus)
                return nsnull;
            dbus_connection_set_exit_on_disconnect(mSessionBus, PR_FALSE);
            dbus_connection_setup_with_g_main(mSessionBus, NULL);
        }
        return mSessionBus;
    }
    return nsnull;
}

DBusService *
DBusService::GetSingleton()
{
    BDBLOG(("DBusService::GetSingleton() called: "));

    if (!gDBusService)
    {
        BDBLOG(("creating new DBusService\n"));
        gDBusService = new DBusService();
    }

    if (gDBusService)
    {
        BDBLOG(("adding reference to existing DBusService\n"));
        NS_ADDREF(gDBusService);
    }

    return gDBusService;
}



//
// Module implementation
//

NS_GENERIC_FACTORY_SINGLETON_CONSTRUCTOR(DBusService, DBusService::GetSingleton);
NS_GENERIC_FACTORY_CONSTRUCTOR(DBusDataCarrier);

static const nsModuleComponentInfo components[] =
{
    {
        "DBus service",
        DBUSSERVICE_CID,
        "@movial.com/dbus/service;1",
        DBusServiceConstructor
    },
    {
        "DBus method",
        DBUSMETHOD_CID,
        "@movial.com/dbus/method;1",
        nsnull
    },
    {
        "DBus signal",
        DBUSSIGNAL_CID,
        "@movial.com/dbus/signal;1",
        nsnull
    },
    {
        "DBus data carrier",
        DBUSDATACARRIER_CID,
        "@movial.com/dbus/datacarrier;1",
        DBusDataCarrierConstructor
    }
};

NS_IMPL_NSGETMODULE(nsDBusServiceModule, components);

/* vim: set cindent ts=4 et sw=4: */
