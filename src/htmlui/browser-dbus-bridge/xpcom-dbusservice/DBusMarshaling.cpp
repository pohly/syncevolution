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

#include "nsEmbedString.h"
#include "nsComponentManagerUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsArrayUtils.h"
#include "nsTArray.h"
#include "nsMemory.h"
#include "nsISupportsPrimitives.h"
#include "nsIProperties.h"
#include "nsIXPConnect.h"

#include "DBusMarshaling.h"
#include "DBusDataCarrier.h"

#include "bdb-debug.h"

void getSignatureFromJSValue(JSContext *cx, jsval *aValue, nsCString &aResult);
void getSignatureFromVariantType(PRUint16 aType, nsCString &aResult);
void getSignatureFromISupports(JSContext* cx, nsISupports *aISupports, nsCString &aResult);

void addArrayToIter(nsIVariant *aVariant, DBusMessageIter *aIter, int aDBusType);
void addArrayDataToIter(JSContext* cx, void *data_ptr, PRUint32 start, PRUint32 count, PRUint16 type, DBusMessageIter *aIter, DBusSignatureIter *aSigIter, DBusSignatureIter *containerSigIter);

void addJSValueToIter(JSContext *cx, jsval *aValue, DBusMessageIter *aIter, DBusSignatureIter *aSigIter);
void getJSValueFromIter(JSContext* cx, DBusMessageIter *aIter, int aDBusType, jsval *v);
already_AddRefed<nsIWritableVariant> getVariantFromIter(DBusMessageIter *aIter, int aDBusType);

void addBasicTypeToIter(nsIVariant *aVariant, DBusMessageIter *aIter, int aDBusType);

void
getSignatureFromJSValue(JSContext *cx, jsval *aValue, nsCString &aResult)
{
    aResult.Assign(DBUS_TYPE_INVALID_AS_STRING);

    if (JSVAL_IS_BOOLEAN(*aValue))
        aResult.Assign(DBUS_TYPE_BOOLEAN_AS_STRING);
    else if (JSVAL_IS_INT(*aValue))
        aResult.Assign(DBUS_TYPE_INT32_AS_STRING);
    else if (JSVAL_IS_DOUBLE(*aValue))
        aResult.Assign(DBUS_TYPE_DOUBLE_AS_STRING);
    else if (JSVAL_IS_STRING(*aValue))
        aResult.Assign(DBUS_TYPE_STRING_AS_STRING);
    else if (JSVAL_IS_OBJECT(*aValue) && JS_IsArrayObject(cx, JSVAL_TO_OBJECT(*aValue)))
    {
        // guess element type from first property value

        JSIdArray *props = JS_Enumerate(cx, JSVAL_TO_OBJECT(*aValue));
        if (props)
        {
            BDBLOG(("    got JSIdArray\n"));
            aResult.Assign(DBUS_TYPE_ARRAY_AS_STRING);

            // get key signature from first property name
            jsval propname;
            nsCAutoString tmpsig;
            JS_IdToValue(cx, props->vector[0], &propname);

            jsval propvalue;
            JSString *prop_string = JS_ValueToString(cx, propname);
            if (JS_LookupUCProperty(cx,
                                    JSVAL_TO_OBJECT(*aValue),
                                    JS_GetStringChars(prop_string),
                                    JS_GetStringLength(prop_string),
                                    &propvalue) == JS_TRUE)
            {
                getSignatureFromJSValue(cx, &propvalue, tmpsig);
                aResult.Append(tmpsig);
            }
            else
            {
                // FIXME - could not find property value??
                // assume string to keep signature valid
                aResult.Append(DBUS_TYPE_STRING_AS_STRING);
            }
            JS_DestroyIdArray(cx, props);
        }

    }
    else if (JSVAL_IS_OBJECT(*aValue))
    {
        nsISupports* supports;
        JSClass* clazz;
        JSObject* glob = JSVAL_TO_OBJECT(*aValue);

        clazz = JS_GET_CLASS(cx, JS_GetParent(cx, glob));

        if (!clazz ||
            !(clazz->flags & JSCLASS_HAS_PRIVATE) ||
            !(clazz->flags & JSCLASS_PRIVATE_IS_NSISUPPORTS) ||
            !(supports = (nsISupports*) JS_GetPrivate(cx, glob))) {

            BDBLOG(("  getSignatureFromJSValue: could not find nsISupports inside object, assume dictionary\n"));

            // try to enumerate object properties
            JSIdArray *props = JS_Enumerate(cx, glob);
            if (props)
            {
                BDBLOG(("    got JSIdArray with %i props\n", props->length));
                aResult.Assign(DBUS_TYPE_ARRAY_AS_STRING);
                aResult.Append(DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING);

                // get key signature from first property name
                jsval propname;
                nsCAutoString tmpsig;
                JS_IdToValue(cx, props->vector[0], &propname);
                getSignatureFromJSValue(cx, &propname, tmpsig);
                aResult.Append(tmpsig);

                jsval propvalue;
                JSString *prop_string = JS_ValueToString(cx, propname);
                if (JS_LookupUCProperty(cx,
                                        glob,
                                        JS_GetStringChars(prop_string),
                                        JS_GetStringLength(prop_string),
                                        &propvalue) == JS_TRUE)
                {
                    getSignatureFromJSValue(cx, &propvalue, tmpsig);
                    aResult.Append(tmpsig);
                }
                else
                {
                    // FIXME - could not find property value??
                    // assume string to keep signature valid
                    aResult.Append(DBUS_TYPE_STRING_AS_STRING);
                }
                aResult.Append(DBUS_DICT_ENTRY_END_CHAR_AS_STRING);
                JS_DestroyIdArray(cx, props);
            }

        }
        else
        {
            BDBLOG(("  getSignatureFromJSValue: clazz->name %s\n", clazz->name));
            // test argument for nsIXPConnectWrappedNative
            nsCOMPtr<nsIXPConnectWrappedNative> wrappednative = do_QueryInterface(supports);
            if (wrappednative)
            {
                BDBLOG(("  getSignatureFromJSValue: got nsIXPConnectWrappedNative\n"));
                nsCOMPtr<nsIVariant> variant = do_QueryInterface(wrappednative->Native());
                if (variant)
                {
                    BDBLOG(("    found wrapped variant\n"));
                    getSignatureFromVariant(cx, variant, aResult);
                    return;
                }
            }
            // use string type as fallback
            aResult.Assign(DBUS_TYPE_STRING_AS_STRING);
            return;
        }

    }
}

void
getSignatureFromVariantType(PRUint16 aType, nsCString &aResult)
{
    switch (aType) {
        case nsIDataType::VTYPE_BOOL:
            aResult.Assign(DBUS_TYPE_BOOLEAN_AS_STRING);
            return;
        case nsIDataType::VTYPE_INT8: /* FIXME - check sign issues;
                                         dbus supports only unsigned 8bit */
        case nsIDataType::VTYPE_UINT8:
            aResult.Assign(DBUS_TYPE_BYTE_AS_STRING);
            return;
        case nsIDataType::VTYPE_INT16:
            aResult.Assign(DBUS_TYPE_INT16_AS_STRING);
            return;
        case nsIDataType::VTYPE_UINT16:
            aResult.Assign(DBUS_TYPE_UINT16_AS_STRING);
            return;
        case nsIDataType::VTYPE_INT32:
            aResult.Assign(DBUS_TYPE_INT32_AS_STRING);
            return;
        case nsIDataType::VTYPE_UINT32:
            aResult.Assign(DBUS_TYPE_UINT32_AS_STRING);
            return;
        case nsIDataType::VTYPE_INT64:
            aResult.Assign(DBUS_TYPE_INT64_AS_STRING);
            return;
        case nsIDataType::VTYPE_UINT64:
            aResult.Assign(DBUS_TYPE_UINT64_AS_STRING);
            return;
        case nsIDataType::VTYPE_DOUBLE:
            aResult.Assign(DBUS_TYPE_DOUBLE_AS_STRING);
            return;
        case nsIDataType::VTYPE_VOID:
            // FIXME - assume that string is the best representation
        case nsIDataType::VTYPE_WSTRING_SIZE_IS:
        case nsIDataType::VTYPE_WCHAR_STR:
            aResult.Assign(DBUS_TYPE_STRING_AS_STRING);
            return;
        default:
            BDBLOG(("  getSignatureFromVariantType: %d not a simple type\n", aType));
            aResult.Assign(DBUS_TYPE_INVALID_AS_STRING);
    }
}

void
getSignatureFromVariant(JSContext* cx, nsIVariant *aVariant, nsCString &aResult)
{
    aResult.Assign(DBUS_TYPE_INVALID_AS_STRING);

    PRUint16 dataType;
    aVariant->GetDataType(&dataType);

    switch (dataType) {
        case nsIDataType::VTYPE_VOID:
        case nsIDataType::VTYPE_BOOL:
        case nsIDataType::VTYPE_INT8:
        case nsIDataType::VTYPE_UINT8:
        case nsIDataType::VTYPE_INT16:
        case nsIDataType::VTYPE_UINT16:
        case nsIDataType::VTYPE_INT32:
        case nsIDataType::VTYPE_UINT32:
        case nsIDataType::VTYPE_INT64:
        case nsIDataType::VTYPE_UINT64:
        case nsIDataType::VTYPE_DOUBLE:
        case nsIDataType::VTYPE_WSTRING_SIZE_IS:
        case nsIDataType::VTYPE_WCHAR_STR:
        {
            PRUint32 val = 0;
            aVariant->GetAsUint32(&val);
            BDBLOG(("  getSignatureFromVariant: simple type %i:%i\n", dataType, val));
            getSignatureFromVariantType(dataType, aResult);
            break;
        }
        case nsIDataType::VTYPE_ARRAY:
        {
            BDBLOG(("  getSignatureFromVariant: array\n"));

            // need to recurse into array
            PRUint16 type = 0;
            nsIID iid;
            PRUint32 count = 0;
            void *data_ptr = nsnull;

            aVariant->GetAsArray(&type,
                                 &iid,
                                 &count,
                                 &data_ptr);

            BDBLOG(("  getSignatureFromVariant: got %d elements of type %d\n", count, type));

            nsCAutoString elementsig;

            if (type == nsIDataType::VTYPE_INTERFACE_IS)
            {
                // get element signature from first element
                nsISupports *element = ((nsISupports **)data_ptr)[0];
                getSignatureFromISupports(cx, element, elementsig);
                for (PRUint32 i = 0; i < count; i++)
                    NS_IF_RELEASE(((nsISupports **)data_ptr)[i]);

            }
            else if (type == nsIDataType::VTYPE_WCHAR_STR)
            {
                getSignatureFromVariantType(type, elementsig);
                for (PRUint32 i = 0; i < count; i++)
                    nsMemory::Free(((char**)data_ptr)[i]);
            }
            else
            {
                getSignatureFromVariantType(type, elementsig);
            }

            aResult.Assign(DBUS_TYPE_ARRAY_AS_STRING);
            aResult.Append(elementsig);

            nsMemory::Free(data_ptr);
            break;
        }
        case nsIDataType::VTYPE_INTERFACE_IS:
        {
            BDBLOG(("  getSignatureFromVariant: interface\n"));
            nsCOMPtr<nsISupports> is;
            nsIID *iid;
            aVariant->GetAsInterface(&iid, getter_AddRefs(is));
            getSignatureFromISupports(cx, is, aResult);
            break;
        }
        default:
        {
            BDBLOG(("  getSignatureFromVariant: unknown type %d\n", dataType));
            break;
        }
    }
}

void
getSignatureFromISupports(JSContext* cx, nsISupports *aISupports, nsCString &aResult)
{
    aResult.Assign(DBUS_TYPE_INVALID_AS_STRING);

    // test argument for nsIVariant
    nsCOMPtr<nsIVariant> variant = do_QueryInterface(aISupports);
    if (variant)
    {
        BDBLOG(("  getSignatureFromISupports: nsIVariant\n"));
        getSignatureFromVariant(cx, variant, aResult);
        return;
    }

    // test argument for DBusDataCarrier
    nsCOMPtr<DBusDataCarrier> carrier = do_QueryInterface(aISupports);
    if (carrier)
    {
        BDBLOG(("  getSignatureFromISupports: DBusDataCarrier\n"));
        carrier->GetType(aResult);
        if (aResult.Equals("r")) {
          nsIVariant *value;
          carrier->GetValue(&value);
          getSignatureFromVariant(cx, value, aResult);
          NS_RELEASE(value);
        } else if (aResult.Equals("v")) {
          carrier->GetSignature(aResult);
        }
        return;
    }

    // test argument for nsIXPConnectWrappedJS
    nsCOMPtr<nsIXPConnectWrappedJS> wrapped = do_QueryInterface(aISupports);
    if (wrapped)
    {
        BDBLOG(("  getSignatureFromISupports: nsIXPConnectWrappedJS\n"));
        JSObject *js_obj = nsnull;
        if (NS_SUCCEEDED(wrapped->GetJSObject(&js_obj)))
        {
            jsval obj_as_jsval = OBJECT_TO_JSVAL(js_obj);
            getSignatureFromJSValue(cx, &obj_as_jsval, aResult);
        }
    }
}

PRUint16 getVType(int dType)
{
    switch (dType)
    {
       case DBUS_TYPE_BOOLEAN:
            return nsIDataType::VTYPE_BOOL;
       case DBUS_TYPE_BYTE:
            return nsIDataType::VTYPE_INT8;
       case DBUS_TYPE_INT16:
            return nsIDataType::VTYPE_INT16;
       case DBUS_TYPE_UINT16:
            return nsIDataType::VTYPE_UINT16;
       case DBUS_TYPE_INT32:
            return nsIDataType::VTYPE_INT32;
       case DBUS_TYPE_UINT32:
            return nsIDataType::VTYPE_UINT32;
       case DBUS_TYPE_DOUBLE:
            return nsIDataType::VTYPE_DOUBLE;
       case DBUS_TYPE_STRING:
       case DBUS_TYPE_OBJECT_PATH:
       case DBUS_TYPE_SIGNATURE:
            return nsIDataType::VTYPE_WCHAR_STR;
       default:
            break;
    }

    return -1;
}

PRBool typesMatch(PRUint16 vType, int dType)
{
    return (vType == getVType(dType));
}

void
addVariantToIter(JSContext* cx, nsIVariant *aVariant, DBusMessageIter *aIter, DBusSignatureIter *aSigIter)
{
    int element_type = dbus_signature_iter_get_current_type(aSigIter);

    PRUint16 variant_type;
    aVariant->GetDataType(&variant_type);

#ifdef DEBUG
    char *element_signature = dbus_signature_iter_get_signature(aSigIter);
    BDBLOG(("addVariantToIter: signature \"%s\", type %c, variant type: %i\n",
           element_signature,
           element_type,
           variant_type));
    dbus_free(element_signature);
#endif

    // If the carrier has a nsISupports, check for DataCarrier
    if (variant_type == nsIDataType::VTYPE_INTERFACE_IS)
    {
        nsCOMPtr<nsISupports> is;
        nsIID *iid;

        if (NS_FAILED(aVariant->GetAsInterface(&iid, getter_AddRefs(is))))
            return;

        nsCOMPtr<DBusDataCarrier> myCarrier = do_QueryInterface(is);
        if (myCarrier) {
            nsIVariant *myValue;
            myCarrier->GetValue(&myValue);
            addVariantToIter(cx, myValue, aIter, aSigIter);
            NS_IF_RELEASE(myValue);
            return;
        }
    }


    if (dbus_type_is_basic(element_type))
    {
        BDBLOG(("  add basic type from variant\n"));
        addBasicTypeToIter(aVariant, aIter, element_type);
    }
    else if (element_type == DBUS_TYPE_ARRAY)
    {
        if (dbus_signature_iter_get_element_type(aSigIter) == DBUS_TYPE_DICT_ENTRY)
        {
            /* TODO: Support for non-JS Dicts */
            
            BDBLOG(("  add dict from variant\n"));

            nsCOMPtr<nsISupports> is;
            nsIID *iid;
            // Reported by a leak, dunno why?
            // It's a comptr so it should go away with the end of context afaik
            aVariant->GetAsInterface(&iid, getter_AddRefs(is));

            // test argument for nsIXPConnectWrappedJS
            nsCOMPtr<nsIXPConnectWrappedJS> wrapped = do_QueryInterface(is);
            if (wrapped)
            {
                BDBLOG(("  Found XPConnect object\n"));
                JSObject *js_obj = nsnull;
                if (NS_SUCCEEDED(wrapped->GetJSObject(&js_obj)))
                {
                    // try to enumerate object properties
                    JSIdArray *props = JS_Enumerate(cx, js_obj);
                    if (props)
                    {
                        BDBLOG(("    got JSIdArray with %i props\n", props->length));
                        
                        // Start the array container
                        DBusMessageIter childIter;
                        DBusSignatureIter childSigIter;
                        DBusSignatureIter dictSigIter;
                        dbus_signature_iter_recurse(aSigIter, &childSigIter);
                        char *array_signature = dbus_signature_iter_get_signature(&childSigIter);
                        dbus_message_iter_open_container(aIter, DBUS_TYPE_ARRAY,
                                                         array_signature, &childIter);
                        dbus_free(array_signature);

                        // Skip the dict signature iter to the value type
                        dbus_signature_iter_recurse(&childSigIter, &dictSigIter);
                        dbus_signature_iter_next(&dictSigIter); // key type

                        nsresult rv;
                        nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
                        if(NS_FAILED(rv))
                            return;
                        BDBLOG(("    got nsIXPConnect\n"));


                        for (int p = 0; p < props->length; p++)
                        {
                            jsval propname;
                            JS_IdToValue(cx, props->vector[p], &propname);

                            // Start the dict container
                            DBusMessageIter dictIter;
                            dbus_message_iter_open_container(&childIter, DBUS_TYPE_DICT_ENTRY,
                                                             NULL, &dictIter);

                            JSString *prop_string = JS_ValueToString(cx, propname);
                            nsCAutoString u8str = NS_ConvertUTF16toUTF8(JS_GetStringChars(prop_string),
                                                                        JS_GetStringLength(prop_string));
                            const char *cstr = u8str.get();
                            // TODO: we only use strings as keys currently, although
                            // the spec allows any basic type to be a key and we
                            // probably *could* use the property index.
                            dbus_message_iter_append_basic(&dictIter,
                                                           DBUS_TYPE_STRING,
                                                           &cstr);

                            jsval propvalue;
                            if (JS_LookupUCProperty(cx,
                                                    js_obj,
                                                    JS_GetStringChars(prop_string),
                                                    JS_GetStringLength(prop_string),
                                                    &propvalue) == JS_TRUE)
                            {
                                nsIVariant *var = nsnull;
                                nsresult rs = xpc->JSToVariant(cx, propvalue, &var);
                                NS_ENSURE_SUCCESS(rs, );

                                addVariantToIter(cx, var, &dictIter, &dictSigIter);
                                NS_IF_RELEASE(var);
                            }
                            
                            // Close the dict entry container
                            dbus_message_iter_close_container(&childIter, &dictIter);
                        }

                        // Close the array container
                        dbus_message_iter_close_container(aIter, &childIter);

                        JS_DestroyIdArray(cx, props);
                    }
                }
            }
        } else {
            BDBLOG(("  add array from variant\n"));

            // need to recurse into array
            PRUint16 type = 0;
            nsIID iid;
            PRUint32 count = 0;
            void *data_ptr = nsnull;

            DBusSignatureIter aChildSigIter;
            dbus_signature_iter_recurse(aSigIter, &aChildSigIter);

            char *array_signature = dbus_signature_iter_get_signature(&aChildSigIter);

            aVariant->GetAsArray(&type,
                                 &iid,
                                 &count,
                                 &data_ptr);

            BDBLOG(("  %s: got %d elements of type %d\n", __FUNCTION__, count, type));
            BDBLOG(("  %s: got array signature %s\n", __FUNCTION__, array_signature));

            DBusMessageIter arrayIter;
            if (!dbus_message_iter_open_container(aIter, DBUS_TYPE_ARRAY,
                                                  array_signature, &arrayIter))
            {
                nsMemory::Free(data_ptr);
                dbus_free(array_signature);
                return;
            }

            addArrayDataToIter(cx, data_ptr, 0, count, type,
                                &arrayIter, &aChildSigIter, aSigIter);

            dbus_message_iter_close_container(aIter, &arrayIter);
            nsMemory::Free(data_ptr);
            dbus_free(array_signature);
        }
    }
    else if (element_type == DBUS_TYPE_VARIANT)
    {
        BDBLOG(("  add variant from variant\n"));

        nsCAutoString variantSignature;
        getSignatureFromVariant(cx, aVariant, variantSignature);

        BDBLOG(("  variant sig: %s\n", variantSignature.get()));

        DBusSignatureIter aChildSigIter;
        dbus_signature_iter_init(&aChildSigIter, variantSignature.get());

        DBusMessageIter variantIter;
        dbus_message_iter_open_container(aIter, DBUS_TYPE_VARIANT,
                                         variantSignature.get(), &variantIter);
        addVariantToIter(cx, aVariant, &variantIter, &aChildSigIter);
        dbus_message_iter_close_container(aIter, &variantIter);

    }
    else if (element_type == DBUS_TYPE_STRUCT)
    {
        BDBLOG(("  add struct from variant\n"));

        if (variant_type != nsIDataType::VTYPE_ARRAY)
        {
            BDBLOG(("  struct not presented as array!\n"));
            return;
        }

        DBusSignatureIter aChildSigIter;
        dbus_signature_iter_recurse(aSigIter, &aChildSigIter);

        char *signature = dbus_signature_iter_get_signature(aSigIter);
        DBusMessageIter structIter;
        dbus_message_iter_open_container(aIter, DBUS_TYPE_STRUCT,
                                         NULL, &structIter);
        BDBLOG(("  struct sig: %s\n", signature));
        dbus_free(signature);

        // Structs are just mixed-type arrays really
        PRUint16 type = 0;
        nsIID iid;
        PRUint32 count = 0;
        void *data_ptr = nsnull;

        aVariant->GetAsArray(&type,
                                &iid,
                                &count,
                                &data_ptr);

        addArrayDataToIter(cx, data_ptr, 0, count, type,
                            &structIter, &aChildSigIter, aSigIter);

        dbus_message_iter_close_container(aIter, &structIter);
        nsMemory::Free(data_ptr);
    }
    else
    {
        BDBLOG(("  unhandled\n"));
    }
}

static
int is_valid_path (const char *path)
{
  const char *cur = path;
  const char *prev = cur;
  
  if (strlen(path) == 0)
    return FALSE;
  
  /* MUST begin with zero */
  if (*cur++ != '/')
    return FALSE;
  
  /* The path is guranteed to be null-terminated */
  while (*cur != '\0')
  {
    /* Two slashes can't be together */
    if (*cur == '/' && *prev == '/')
    {
      return FALSE;
    } else if (!(((*cur) >= '0' && (*cur) <= '9') ||
                 ((*cur) >= 'A' && (*cur) <= 'Z') ||
                 ((*cur) >= 'a' && (*cur) <= 'z') ||
                  (*cur) == '_' || (*cur) == '/')) {
      return FALSE;
    }
    prev = cur;
    cur++;
  }
  
  return TRUE;
}


void addBasicTypeToIter(nsIVariant *aVariant, DBusMessageIter *aIter, int aDBusType)
{

    PRUint16 dataType;
    aVariant->GetDataType(&dataType);

    /* If we got passed an nsISupports, query the variant iface from it and recurse */
    if (dataType == nsIDataType::VTYPE_INTERFACE_IS)
    {
        nsCOMPtr<nsISupports> is;
        nsIID *iid;
        if (NS_FAILED(aVariant->GetAsInterface(&iid, getter_AddRefs(is))))
            return;

        nsCOMPtr<nsIVariant> myVariant = do_QueryInterface(is);
        if (myVariant) {
            addBasicTypeToIter(myVariant, aIter, aDBusType);
            return;
        }

        nsCOMPtr<DBusDataCarrier> myCarrier = do_QueryInterface(is);
        if (myCarrier) {
            nsCOMPtr<nsIVariant> myValue;
            myCarrier->GetValue(getter_AddRefs(myValue));
            addBasicTypeToIter(myValue, aIter, aDBusType);
            return;
        }

        BDBLOG(("  Got nsISupports, but don't know what to do with it!\n"));

        return;
    }

    switch (aDBusType)
    {
        case DBUS_TYPE_BOOLEAN:
        {
            PRBool val;
            if (NS_FAILED(aVariant->GetAsBool(&val)))
              return;
            BDBLOG(("  arg       : BOOLEAN %s\n", val ? "true" : "false"));
            dbus_message_iter_append_basic(aIter,
                                           aDBusType,
                                           &val);
            break;
        }
        case DBUS_TYPE_BYTE:
        case DBUS_TYPE_INT16:
        case DBUS_TYPE_UINT16:
        case DBUS_TYPE_INT32:
        case DBUS_TYPE_UINT32:
        {
            PRUint32 val;
            if (NS_FAILED(aVariant->GetAsUint32(&val)))
              return;

            BDBLOG(("  arg       : INT(8|16|32) (%c) %d:%d\n", aDBusType, dataType, val));
            dbus_message_iter_append_basic(aIter,
                                           aDBusType,
                                           &val);
            break;
        }
        case DBUS_TYPE_INT64:
        {
            PRInt64 val = 0;
            if (NS_FAILED(aVariant->GetAsInt64(&val)))
                return;
            BDBLOG(("  arg       : INT64 %lld\n", val));
            dbus_message_iter_append_basic(aIter,
                                           aDBusType,
                                           &val);
            break;
        }
        case DBUS_TYPE_UINT64:
        {
            PRUint64 val;
            if (NS_FAILED(aVariant->GetAsUint64(&val)))
                return;
            BDBLOG(("  arg       : UINT64 %llu\n", val));
            dbus_message_iter_append_basic(aIter,
                                           aDBusType,
                                           &val);
            break;
        }
        case DBUS_TYPE_DOUBLE:
        {
            double val;
            if (NS_FAILED(aVariant->GetAsDouble(&val)))
                return;
            BDBLOG(("  arg       : DOUBLE (%c) %f\n", aDBusType, val));
            dbus_message_iter_append_basic(aIter,
                                           aDBusType,
                                           &val);
            break;
        }
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
        {
            /* FIXME - handle utf-8 conversion */
            nsCAutoString val;
            const char *val_ptr;
            if (NS_FAILED(aVariant->GetAsAUTF8String(val)))
                return;

            val_ptr = PromiseFlatCString(val).get();
            BDBLOG(("  arg       : STRING '%s'\n", val_ptr));
            if (aDBusType == DBUS_TYPE_OBJECT_PATH
             && !is_valid_path(val_ptr))
                return;

            dbus_message_iter_append_basic(aIter, aDBusType, &val_ptr);
            break;
        }
        default:
        {
            BDBLOG(("  addBasicTypeToIter: unhandled DBus type %d!\n", aDBusType));
            break;
        }
    }
}

void addArrayDataToIter(JSContext* cx, void *data_ptr, PRUint32 start, PRUint32 count, PRUint16 type, DBusMessageIter *aIter, DBusSignatureIter *aSigIter, DBusSignatureIter *containerSigIter)
{
    int aDBusType = dbus_signature_iter_get_current_type(aSigIter);
    BDBLOG(("addArrayDataToIter: appending %d elements of type %d '%c'\n", count, type, aDBusType));

    switch (type)
    {
#define ADD_DATA \
            for (PRUint32 i = start; i < count; i++) \
                dbus_message_iter_append_basic(aIter, aDBusType, data+i)
#define ADD_DATA_AS_DOUBLE  do { \
            for (PRUint32 i = start; i < count; i++) { \
                double t = *(data+i); \
                dbus_message_iter_append_basic(aIter, aDBusType, &t); \
            } } while (0)
        case nsIDataType::VTYPE_BOOL:
        {
            PRBool *data = (PRBool *)data_ptr;
            ADD_DATA;
            break;
        }
        case nsIDataType::VTYPE_INT8:
        case nsIDataType::VTYPE_UINT8:
        {
            char *data = (char *)data_ptr;
            if (aDBusType == DBUS_TYPE_DOUBLE)
                ADD_DATA_AS_DOUBLE;
            else
                ADD_DATA;
            break;
        }
        case nsIDataType::VTYPE_INT16:
        case nsIDataType::VTYPE_UINT16:
        {
            PRInt16 *data = (PRInt16 *)data_ptr;
            if (aDBusType == DBUS_TYPE_DOUBLE)
                ADD_DATA_AS_DOUBLE;
            else
                ADD_DATA;
            break;
        }
        case nsIDataType::VTYPE_INT32:
        case nsIDataType::VTYPE_UINT32:
        {
            PRInt32 *data = (PRInt32 *)data_ptr;
            if (aDBusType == DBUS_TYPE_DOUBLE)
                ADD_DATA_AS_DOUBLE;
            else
                ADD_DATA;
            break;
        }
        case nsIDataType::VTYPE_INT64:
        case nsIDataType::VTYPE_UINT64:
        {
            PRInt64 *data = (PRInt64 *)data_ptr;
            if (aDBusType == DBUS_TYPE_DOUBLE)
                ADD_DATA_AS_DOUBLE;
            else
                ADD_DATA;
            break;
        }
        case nsIDataType::VTYPE_DOUBLE:
        {
            double *data = (double *)data_ptr;
            ADD_DATA;
            break;
        }
        case nsIDataType::VTYPE_WCHAR_STR:
        {
            PRUnichar **data = (PRUnichar **)data_ptr;
            for (PRUint32 i = start; i < count; i++)
            {
                const char *val_ptr;
                nsCAutoString val = NS_ConvertUTF16toUTF8(data[i]);

                val_ptr = PromiseFlatCString(val).get();
                BDBLOG(("  arg       : STRING '%s'\n", val_ptr));
                if (aDBusType == DBUS_TYPE_OBJECT_PATH
                 && !is_valid_path(val_ptr))
                    return;

                dbus_message_iter_append_basic(aIter, aDBusType, &val_ptr);
            }
            break;
        }
        case nsIDataType::VTYPE_INTERFACE_IS:
        {
            DBusSignatureIter childSigIter;
            dbus_signature_iter_recurse(containerSigIter, &childSigIter);

            nsISupports **data = (nsISupports **)data_ptr;
            for (PRUint32 i = 0; i < count; i++)
            {

                // We might have a wrapped JS object in the nsISupports
                // eg. dicts
                nsCOMPtr<nsIXPConnectWrappedJS> wrapped = do_QueryInterface(data[i]);

                if (wrapped) {
                    JSObject *js_obj = nsnull;
                    if (!NS_SUCCEEDED(wrapped->GetJSObject(&js_obj)))
                        continue;

                    jsval js_obj_as_value = OBJECT_TO_JSVAL(js_obj);
                    addJSValueToIter(cx, &js_obj_as_value,
                                        aIter, &childSigIter);
                } else {

                    // We might have a variant
                    nsCOMPtr<nsIVariant> variant = do_QueryInterface(data[i]);

                    if (variant)
                        addVariantToIter(cx, variant, aIter, &childSigIter);
                }

                /* Advance the signature iter or reset */
                if (!dbus_signature_iter_next(&childSigIter))
                    dbus_signature_iter_recurse(containerSigIter, &childSigIter);

            }

            break;
        }
        default:
        {
            BDBLOG(("addArrayDataToIter: unhandled array data type %d\n", type));
            break;
        }
#undef ADD_DATA
    }
}

void
addJSValueToIter(JSContext *cx, jsval *aValue, DBusMessageIter *aIter, DBusSignatureIter *aSigIter)
{

    int dbusType = dbus_signature_iter_get_current_type(aSigIter);

    BDBLOG(("%s(%s, %c, %s)\n", __FUNCTION__,
            JS_GetTypeName(cx, JS_TypeOfValue(cx, *aValue)),
            dbusType, dbus_signature_iter_get_signature(aSigIter)));

    // Using the expected type instead of the actual allows autoconversion
    switch (dbusType)
    {
        case DBUS_TYPE_BOOLEAN:
        {
            JSBool b = JS_FALSE;
            if (JS_ValueToBoolean(cx, *aValue, &b))
            {
                dbus_message_iter_append_basic(aIter, DBUS_TYPE_BOOLEAN, &b);
            }
            else
            {
                BDBLOG(("%s(): Could not fetch boolean from jsvalue\n", __FUNCTION__));
            }
            
            
            break;
        }
        case DBUS_TYPE_BYTE:
        case DBUS_TYPE_INT16:
        case DBUS_TYPE_UINT16:
        case DBUS_TYPE_INT32:
        case DBUS_TYPE_UINT32:
        case DBUS_TYPE_INT64:
        case DBUS_TYPE_UINT64:
        case DBUS_TYPE_DOUBLE:
        {
            jsdouble d = 0;
            
            if (JS_ValueToNumber(cx, *aValue, &d))
            {
                BDBLOG(("%s(%f)\n", __FUNCTION__, d));
                dbus_message_iter_append_basic(aIter, dbusType, &d);
            }
            else
            {
                BDBLOG(("%s(): Could not fetch number from jsvalue\n", __FUNCTION__));
            }

            break;
        }
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
        {
            JSString *prop_string = JS_ValueToString(cx, *aValue);
            const char *cstr = NS_ConvertUTF16toUTF8(JS_GetStringChars(prop_string),
                                                     JS_GetStringLength(prop_string)).get();
            dbus_message_iter_append_basic(aIter, dbusType, &cstr);
            break;
        }
        case DBUS_TYPE_ARRAY:
        {
            if (!JSVAL_IS_OBJECT(*aValue))
                break;

            if (JS_IsArrayObject(cx, JSVAL_TO_OBJECT(*aValue))) {
                // We iterate the JS arrays here to (potentially) avoid
                // extra conversions to variants

                JSObject *array = JSVAL_TO_OBJECT(*aValue);
                jsuint length = 0;
                if (!JS_GetArrayLength(cx, array, &length))
                    break;

                DBusSignatureIter aChildSigIter;
                dbus_signature_iter_recurse(aSigIter, &aChildSigIter);

                char *array_signature = dbus_signature_iter_get_signature(&aChildSigIter);

                BDBLOG(("  %s: got array signature %s\n", __FUNCTION__, array_signature));

                DBusMessageIter arrayIter;
                if (!dbus_message_iter_open_container(aIter, DBUS_TYPE_ARRAY,
                                                        array_signature, &arrayIter))
                {
                    dbus_free(array_signature);
                    break;
                }
                dbus_free(array_signature);

                for (jsuint e = 0; e < length; e++)
                {
                    jsval ev;
                    if (JS_GetElement(cx, array, e, &ev))
                        addJSValueToIter(cx, &ev, &arrayIter, &aChildSigIter);
                }

                dbus_message_iter_close_container(aIter, &arrayIter);

            } else {
                // non-array JS objects are converted to variants and pushed
                // to the variant code path
                nsresult rv;
                nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
                if(NS_FAILED(rv))
                    return;
                BDBLOG(("    got nsIXPConnect\n"));

                nsIVariant *var = nsnull;
                nsresult rs = xpc->JSToVariant(cx, *aValue, &var);
                NS_ENSURE_SUCCESS(rs, );

                addVariantToIter(cx, var, aIter, aSigIter);
                NS_IF_RELEASE(var);
            }
            
            break;
        }
        default:
            BDBLOG(("Don't know how to convert type '%c'\n", dbus_signature_iter_get_current_type(aSigIter)));
            break;
   }
}

void getDictFromArray(JSContext* cx, DBusMessageIter *arrayIter, JSObject **obj)
{
    *obj = JS_NewObject(cx, nsnull, nsnull, nsnull);

    do
    {
        DBusMessageIter dict_iter;
        char *key = nsnull;
        dbus_message_iter_recurse(arrayIter, &dict_iter);
        dbus_message_iter_get_basic(&dict_iter, &key);
        BDBLOG(("    found key %s\n", key ? key : "null"));
        dbus_message_iter_next(&dict_iter);
        int value_type = dbus_message_iter_get_arg_type(&dict_iter);
        BDBLOG(("    found value type %c\n", value_type));
        jsval v;
        getJSValueFromIter(cx, &dict_iter, value_type, &v);
        nsAutoString ukey = NS_ConvertUTF8toUTF16(key);
        JS_SetUCProperty(cx, *obj, ukey.get(), ukey.Length(), &v);

    } while (dbus_message_iter_next(arrayIter));

}

void getJSArrayFromIter(JSContext* cx, DBusMessageIter *aIter, JSObject **array)
{
    nsTArray<jsval> elems;

    // iterate over array elements
    do
    {
        jsval cv;
        BDBLOG(("arg type: %c\n",
                dbus_message_iter_get_arg_type(aIter)));
        getJSValueFromIter(cx, aIter,
                           dbus_message_iter_get_arg_type(aIter),
                           &cv);
        elems.AppendElement(cv);

    } while (dbus_message_iter_next(aIter));

    // Create an Array object with the elements
    *array = JS_NewArrayObject(cx, elems.Length(), elems.Elements());
}

void getJSValueFromIter(JSContext* cx, DBusMessageIter *aIter, int aDBusType, jsval *v)
{

    BDBLOG(("%s(%c)\n", __FUNCTION__, aDBusType));


    switch (aDBusType)
    {
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
        {
            char *val = nsnull;
            dbus_message_iter_get_basic(aIter, &val);
            if (val != nsnull)
            {
                nsAutoString uval = NS_ConvertUTF8toUTF16(val);
                JSString *str = JS_NewUCStringCopyN(cx, uval.get(), uval.Length());
                *v = STRING_TO_JSVAL(str);
            }
            break;
        }
        case DBUS_TYPE_BYTE:
        case DBUS_TYPE_INT16:
        case DBUS_TYPE_UINT16:
        case DBUS_TYPE_INT32:
        case DBUS_TYPE_UINT32:
        case DBUS_TYPE_INT64:
        case DBUS_TYPE_UINT64:
        {
            dbus_uint64_t val = 0;
            dbus_message_iter_get_basic(aIter, &val);
            if (!JS_NewNumberValue(cx, (jsdouble)val, v))
            {
                BDBLOG(("%s: Number conversion from %c failed\n", __FUNCTION__,
                       aDBusType));
            }
            break;
        }
        case DBUS_TYPE_DOUBLE:
        {
            jsdouble val = 0.0;
            dbus_message_iter_get_basic(aIter, &val);
            if (!JS_NewNumberValue(cx, val, v))
            {
                BDBLOG(("%s: Number conversion from %c failed\n", __FUNCTION__,
                       aDBusType));
            }
            break;
        }
        case DBUS_TYPE_ARRAY:
        {
            DBusMessageIter arrayIter;
            dbus_message_iter_recurse(aIter, &arrayIter);

            if (dbus_message_iter_get_element_type(aIter) == DBUS_TYPE_DICT_ENTRY)
            {
                BDBLOG(("    arg type ARRAY with DICT_ENTRY\n"));

                JSObject *obj = nsnull;
                getDictFromArray(cx, &arrayIter, &obj);

                *v = OBJECT_TO_JSVAL(obj);
            } else {
                JSObject *array = nsnull;
                getJSArrayFromIter(cx, &arrayIter, &array);
                *v = OBJECT_TO_JSVAL(array);
            }
            break;
        }
        case DBUS_TYPE_VARIANT:
        {
            DBusMessageIter variantIter;
            dbus_message_iter_recurse(aIter, &variantIter);
            getJSValueFromIter(cx, &variantIter,
                               dbus_message_iter_get_arg_type(&variantIter),
                               v);
            break;
        }
        case DBUS_TYPE_STRUCT:
        {
            DBusMessageIter structIter;
            dbus_message_iter_recurse(aIter, &structIter);

            JSObject *array = nsnull;
            getJSArrayFromIter(cx, &structIter, &array);
            *v = OBJECT_TO_JSVAL(array);

            break;
        }
        default:
        {
            BDBLOG(("%s: Unhandled type %c\n", __FUNCTION__, aDBusType));
        }
    }

    return;
}

already_AddRefed<nsIWritableVariant> getVariantFromIter(JSContext* cx, DBusMessageIter *aIter, int aDBusType)
{
    nsCOMPtr<nsIWritableVariant> variant = do_CreateInstance("@mozilla.org/variant;1");
    nsIWritableVariant *retval;

    switch (aDBusType)
    {
        case DBUS_TYPE_BOOLEAN:
        {
            PRUint32 val = 0;
            BDBLOG(("    arg type BOOLEAN: "));
            dbus_message_iter_get_basic(aIter, &val);
            BDBLOG(("%d\n", val));
            variant->SetAsBool(val);
            break;
        }

        case DBUS_TYPE_BYTE:
        case DBUS_TYPE_INT16:
        case DBUS_TYPE_UINT16:
        case DBUS_TYPE_INT32:
        case DBUS_TYPE_UINT32:
        {
            PRUint32 val = 0;
            BDBLOG(("    arg type INT: "));
            dbus_message_iter_get_basic(aIter, &val);
            BDBLOG(("%d\n", val));
            variant->SetAsUint32(val);
            break;
        }
        case DBUS_TYPE_INT64:
        {
            PRInt64 val;
            BDBLOG(("    arg type INT64: "));
            dbus_message_iter_get_basic(aIter, &val);
            BDBLOG(("%lld\n", val));
            variant->SetAsInt64(val);
            break;
        }
        case DBUS_TYPE_UINT64:
        {
            PRUint64 val;
            BDBLOG(("    arg type UINT64: "));
            dbus_message_iter_get_basic(aIter, &val);
            BDBLOG(("%llu\n", val));
            variant->SetAsUint64(val);
            break;
        }
        case DBUS_TYPE_DOUBLE:
        {
            double val;
            BDBLOG(("    arg type DOUBLE: "));
            dbus_message_iter_get_basic(aIter, &val);
            BDBLOG(("%f\n", val));
            variant->SetAsDouble(val);
            break;
        }
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE:
        {
            const char *tmp;
            BDBLOG(("    arg type STRING/OBJECT_PATH/SIGNATURE: "));
            dbus_message_iter_get_basic(aIter, &tmp);
            nsDependentCString val(tmp);
            BDBLOG(("\"%s\"\n", PromiseFlatCString(val).get()));
            variant->SetAsAUTF8String(val);
            break;
        }
        case DBUS_TYPE_ARRAY:
        {
            if (dbus_message_iter_get_element_type(aIter) == DBUS_TYPE_DICT_ENTRY)
            {
                BDBLOG(("    arg type ARRAY with DICT_ENTRY\n"));

                DBusMessageIter array_iter;
                dbus_message_iter_recurse(aIter, &array_iter);

                JSObject *obj = nsnull;
                getDictFromArray(cx, &array_iter, &obj);

                // get the xpconnect service
                nsresult rv;
                nsCOMPtr<nsIXPConnect> xpc(do_GetService(nsIXPConnect::GetCID(), &rv));
                if(NS_FAILED(rv))
                    return nsnull;
                BDBLOG(("    got nsIXPConnect\n"));

                // Convert to variant and return
                nsIVariant *var = nsnull;
                rv = xpc->JSToVariant(cx, OBJECT_TO_JSVAL(obj), &var);
                if(NS_FAILED(rv))
                    return nsnull;
                variant->SetFromVariant(var);
                NS_RELEASE(var);
                var = nsnull;
                NS_ADDREF(retval = variant);
                
                return retval;
            }
            else
            {
        
                DBusMessageIter array_iter;
                nsCOMPtr<nsIMutableArray> items;
                PRUint32 item_count;

                BDBLOG(("    arg type ARRAY\n"));
                dbus_message_iter_recurse(aIter, &array_iter);
                items = getArrayFromIter(cx, &array_iter);
                items->GetLength(&item_count);
                BDBLOG(("    array: %d items\n", item_count));

                nsIVariant **item_array = new nsIVariant*[item_count];
                for (PRUint32 i = 0; i < item_count; i++)
                {
                    nsCOMPtr<nsIVariant> item = do_QueryElementAt(items, i);
                    item_array[i] = item;
                    NS_ADDREF(item_array[i]);
                }
                variant->SetAsArray(nsIDataType::VTYPE_INTERFACE_IS,
                                    &NS_GET_IID(nsIVariant),
                                    item_count,
                                    item_array);
                for (PRUint32 i = 0; i < item_count; i++)
                    NS_RELEASE(item_array[i]);
                delete[] item_array;
            }
            break;
        }
        case DBUS_TYPE_VARIANT:
        {
            BDBLOG(("    arg type VARIANT\n"));

            DBusMessageIter variant_iter;
            dbus_message_iter_recurse(aIter, &variant_iter);

            int childType = dbus_message_iter_get_arg_type(&variant_iter);
            variant = getVariantFromIter(cx, &variant_iter, childType);

            break;
        }
        case DBUS_TYPE_STRUCT:
        {
            BDBLOG(("    arg type STRUCT\n"));

            DBusMessageIter array_iter;
            nsCOMPtr<nsIMutableArray> items;
            PRUint32 item_count;

            dbus_message_iter_recurse(aIter, &array_iter);
            items = getArrayFromIter(cx, &array_iter);
            items->GetLength(&item_count);
            BDBLOG(("    struct: %d items\n", item_count));

            nsIVariant **item_array = new nsIVariant*[item_count];
            for (PRUint32 i = 0; i < item_count; i++)
            {
                nsCOMPtr<nsIVariant> item = do_QueryElementAt(items, i);
                item_array[i] = item;
                NS_ADDREF(item_array[i]);
            }
            variant->SetAsArray(nsIDataType::VTYPE_INTERFACE_IS,
                                &NS_GET_IID(nsIVariant),
                                item_count,
                                item_array);
            for (PRUint32 i = 0; i < item_count; i++)
                NS_RELEASE(item_array[i]);
            delete[] item_array;

            break;
        }
        default:
        {
            BDBLOG(("    arg type '%c' (%d)\n", aDBusType, aDBusType));
            return nsnull;
        }
    }

    NS_ADDREF(retval = variant);
    return retval;
}

already_AddRefed<nsIMutableArray> getArrayFromIter(JSContext* cx, DBusMessageIter *aIter)
{
    int current_type;
    nsCOMPtr<nsIMutableArray> array = do_CreateInstance("@mozilla.org/array;1");
    nsIMutableArray *retval;

    BDBLOG(("  ++ enter getArrayFromIter\n"));

    while ((current_type = dbus_message_iter_get_arg_type(aIter)) != DBUS_TYPE_INVALID)
    {
        nsCOMPtr<nsIWritableVariant> variant = getVariantFromIter(cx, aIter, current_type);
        if (variant)
            array->AppendElement(variant, PR_FALSE);
        else
            BDBLOG(("    arg type '%c' (%d) not handled\n", current_type, current_type));

        dbus_message_iter_next(aIter);
    }
 
    NS_ADDREF(retval = array);
    BDBLOG(("  ++ leave getArrayFromIter\n"));
    return retval;
}

/* vim: set cindent ts=4 et sw=4: */
