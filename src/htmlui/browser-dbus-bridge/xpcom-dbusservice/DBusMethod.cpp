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
#include <dbus/dbus.h>

#include "nsComponentManagerUtils.h"
#include "nsIMutableArray.h"
#include "nsArrayUtils.h"
#include "nsISupportsPrimitives.h"
#include "nsIProperties.h"
#include "nsIXPConnect.h"

#include "IDBusService.h"
#include "DBusMethod.h"
#include "DBusMarshaling.h"

#include "bdb-debug.h"

//
// helper declarations
//

static void DoCallBack(DBusMethod *aCallback, DBusMessage *aReply);
static void ReplyHandler(DBusPendingCall *pending, void *user_data);

//
// DBusMethod implementation
//

NS_IMPL_ISUPPORTS1(DBusMethod, IDBusMethod)

DBusMethod::DBusMethod(DBusService *aDBusService,
                       PRUint32 aBusType,
                       const nsACString& aDestination,
                       const nsACString& aObjectPath,
                       const nsACString& aMethodName,
                       const nsACString& aInterfaceName,
                       const nsACString& aSignature,
                       JSContext *cx) :
    mDBusService(aDBusService),
    mBusType(aBusType),
    mDestination(aDestination),
    mObject(aObjectPath),
    mMethod(aMethodName),
    mInterface(aInterfaceName),
    mSignature(aSignature),
    mAsync(PR_TRUE),
    mCallback(nsnull),
    mErrorCallback(nsnull),
    mJScx(cx)
{
    BDBLOG(("DBusMethod::DBusMethod()\n"));
    BDBLOG(("  aBusType          : %d\n", aBusType));
    BDBLOG(("  aDestination      : %s\n", PromiseFlatCString(aDestination).get()));
    BDBLOG(("  aObjectPath       : %s\n", PromiseFlatCString(aObjectPath).get()));
    BDBLOG(("  aMethodName       : %s\n", PromiseFlatCString(aMethodName).get()));
    BDBLOG(("  aInterfaceName    : %s\n", PromiseFlatCString(aInterfaceName).get()));
    BDBLOG(("  aSignature        : %s\n", PromiseFlatCString(aSignature).get()));

}

DBusMethod::~DBusMethod()
{
    BDBLOG(("DBusMethod::~DBusMethod()\n"));
    if (mCallback)
        NS_RELEASE(mCallback);
    if (mErrorCallback)
        NS_RELEASE(mErrorCallback);
}

NS_IMETHODIMP
DBusMethod::GetAsync(PRBool *aAsync)
{
    *aAsync = mAsync;
    return NS_OK;
}

NS_IMETHODIMP
DBusMethod::SetAsync(PRBool aAsync)
{
    BDBLOG(("DBusMethod::SetAsync(%s)\n", aAsync ? "true" : "false"));
    mAsync = aAsync;
    return NS_OK;
}

NS_IMETHODIMP
DBusMethod::GetOnReply(IDBusMethodCallback **aOnReply)
{
    *aOnReply = mCallback;
    NS_IF_ADDREF(*aOnReply);
    return NS_OK;
}

NS_IMETHODIMP
DBusMethod::SetOnReply(IDBusMethodCallback *aOnReply)
{
    BDBLOG(("DBusMethod::SetOnReply(%08x)\n", (unsigned int)aOnReply));
    if (mCallback)
        NS_RELEASE(mCallback);
    mCallback = aOnReply;
    NS_IF_ADDREF(mCallback);
    return NS_OK;
}

NS_IMETHODIMP
DBusMethod::GetOnError(IDBusMethodCallback **aOnError)
{
    *aOnError = mErrorCallback;
    NS_IF_ADDREF(*aOnError);
    return NS_OK;
}

NS_IMETHODIMP
DBusMethod::SetOnError(IDBusMethodCallback *aOnError)
{
    BDBLOG(("DBusMethod::SetOnError(%08x)\n", (unsigned int)aOnError));
    if (mErrorCallback)
        NS_RELEASE(mErrorCallback);
    mErrorCallback = aOnError;
    NS_IF_ADDREF(mErrorCallback);
    return NS_OK;
}

NS_IMETHODIMP
DBusMethod::GetJSContext(JSContext **aJSContext)
{
    *aJSContext = mJScx;
    return NS_OK;
}

static void
ReplyHandler(DBusPendingCall *pending, void *user_data)
{
    DBusMessage *reply;
    DBusMethod *method = (DBusMethod *) user_data;

    reply = dbus_pending_call_steal_reply(pending);
    DoCallBack(method, reply);
    dbus_message_unref(reply);
    // We took a ref when starting the async call, release it here
    method->Release();
}


static void
DoCallBack(DBusMethod *aMethod, DBusMessage *aReply)
{
    DBusMessageIter iter;
    nsCOMPtr<nsIMutableArray> reply_args;
    nsCOMPtr<IDBusMethodCallback> callback;

    int msg_type = dbus_message_get_type(aReply);

    dbus_message_iter_init(aReply, &iter);

    JSContext *cx;
    aMethod->GetJSContext(&cx);
    reply_args = getArrayFromIter(cx, &iter);

    switch (msg_type)
    {
        case DBUS_MESSAGE_TYPE_METHOD_RETURN:
        {
            BDBLOG(("  got method reply\n"));
            aMethod->GetOnReply(getter_AddRefs(callback));
            break;
        }
        case DBUS_MESSAGE_TYPE_ERROR:
        {
            BDBLOG(("  got an error message: %s\n", dbus_message_get_error_name(aReply)));
            aMethod->GetOnError(getter_AddRefs(callback));

            /* insert error name as first callback argument */
            nsCOMPtr<nsIWritableVariant> error_name = do_CreateInstance("@mozilla.org/variant;1");
            error_name->SetAsString(dbus_message_get_error_name(aReply));
            reply_args->InsertElementAt(error_name, 0, PR_FALSE);

            break;
        }
        default:
        {
            BDBLOG(("  got unhandled message of type %d\n", msg_type));
            break;
        }
    }

    PRUint32 reply_items;
    reply_args->GetLength(&reply_items);
    BDBLOG(("  reply_args: %d items\n", reply_items));

    if (callback)
    {
        /* arguments are packed as an array into an nsIVariant */
        nsIVariant **callback_args = new nsIVariant*[reply_items];
        nsCOMPtr<nsIWritableVariant> args = do_CreateInstance("@mozilla.org/variant;1");
        for (PRUint32 i = 0; i < reply_items; i++)
        {
            nsCOMPtr<nsIVariant> arg = do_QueryElementAt(reply_args, i);
            callback_args[i] = arg;
            NS_ADDREF(callback_args[i]);
        }
        args->SetAsArray(nsIDataType::VTYPE_INTERFACE_IS,
                         &NS_GET_IID(nsIVariant),
                         reply_items,
                         callback_args);
        for (PRUint32 i = 0; i < reply_items; i++)
            NS_RELEASE(callback_args[i]);
        delete[] callback_args;
        callback->OnReply(args);
    }
}

NS_IMETHODIMP
DBusMethod::DoCall(nsIVariant **aArgs, PRUint32 aCount)
{
    DBusMessage *msg;
    DBusMessageIter msg_iter;
    nsCAutoString signature;

    BDBLOG(("DBusMethod::DoCall()\n"));
    BDBLOG(("  aCount          : %d\n", aCount));

    msg = dbus_message_new_method_call(PromiseFlatCString(mDestination).get(),
                                       PromiseFlatCString(mObject).get(),
                                       PromiseFlatCString(mInterface).get(),
                                       PromiseFlatCString(mMethod).get());
    dbus_message_iter_init_append(msg, &msg_iter);

    if (mSignature.Equals(""))
    {
        // FIXME - is it necessary to clear the string?
        signature.Assign("");
        for (PRUint32 i = 0; i < aCount; i++)
        {
            // no method signature specified, guess argument types
            nsCOMPtr<nsIVariant> data = aArgs[i];
            nsCAutoString tmpsig;

            getSignatureFromVariant(mJScx, data, tmpsig);
            BDBLOG(("  aArgs[%02d]       : signature \"%s\"\n",
                   i,
                   PromiseFlatCString(tmpsig).get()));
            signature.Append(tmpsig);

        } /* for (int i = 0; i < aCount; i++) */
    } /* if (mSignature.Equals("")) */
    else
    {
        signature.Assign(mSignature);
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

            addVariantToIter(mJScx, aArgs[i], &msg_iter, &sig_iter);

            i++;
            dbus_free(element_signature);
            dbus_signature_iter_next(&sig_iter);
        }
    }
    else
    {
        BDBLOG(("  invalid signature \"%s\"\n", PromiseFlatCString(signature).get()));
        return NS_ERROR_ILLEGAL_VALUE;
    }

    // Sanity-check: make sure that the signature we think we are sending matches
    // that of the message
    
    if (!signature.Equals(dbus_message_get_signature(msg)))
    {
        BDBLOG(("  signature mismatch! Expected '%s', got '%s'\n",
                PromiseFlatCString(signature).get(),
                dbus_message_get_signature(msg)));
        return NS_ERROR_ILLEGAL_VALUE;
    }

    DBusPendingCall *pending = mDBusService->SendWithReply(mBusType,
                                                           msg,
                                                           -1);
    if (pending)
    {
        if (mAsync)
        {
            BDBLOG(("  do async reply callback\n"));
            if (dbus_pending_call_set_notify(pending,
                                             ReplyHandler,
                                             this,
                                             nsnull))
            {
                // Add a ref to make sure the method object doesn't die
                // before the reply comes.
                this->AddRef();
            }
        }
        else
        {
            DBusMessage *reply;
            BDBLOG(("  do sync reply callback\n"));
            dbus_pending_call_block(pending);
            reply = dbus_pending_call_steal_reply (pending);
            DoCallBack(this, reply);
            dbus_message_unref(reply);
        }
        dbus_pending_call_unref (pending);
    }
    else
    {
        return NS_ERROR_OUT_OF_MEMORY;
    }

    dbus_message_unref(msg);
    return NS_OK;
}


/* vim: set cindent ts=4 et sw=4: */
