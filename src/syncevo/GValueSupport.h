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

#ifndef INCL_GVALUE_SUPPORT
#define INCL_GVALUE_SUPPORT

#include <glib-object.h>
#include <syncevo/GLibSupport.h>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/**
 * Base C++ wrapper for GValue. Owns the data stored in it.
 * init() must be called before using it.
 */
class GValueCXX : public GValue
{
 public:
    GValueCXX() { memset(static_cast<GValue *>(this), 0, sizeof(GValue)); }
    GValueCXX(const GValue &other) {
        memset(static_cast<GValue *>(this), 0, sizeof(GValue));
        *this = other;
    }
    ~GValueCXX() { g_value_unset(this); }

    void init(GType gType) { g_value_init(this, gType); }
    GValueCXX &operator = (const GValue &other) {
        if (&other != this) {
            g_value_copy(&other, this);
        }
        return *this;
    }

    /** text representation, for debugging */
    PlainGStr toString() const {
        PlainGStr str(g_strdup_value_contents(this));
        return str;
    }

    /**
     * A GDestroyNotify for dynamically allocated GValueCXX instances,
     * for use in containers like GHashTable.
     */
    static void destroy(gpointer gvaluecxx) {
        delete static_cast<GValueCXX *>(gvaluecxx);
    }
};

template<class C> void dummyTake(GValue *, C);
template<class C> void dummySetStatic(GValue *, const C);

/**
 * Declares a C++ wrapper for a GValue containing a specific
 * GType.
 */
template<
    class nativeType,
    class constNativeType,
    GType gType,
    void (*setGValue)(GValue *, constNativeType),
    constNativeType (*getFromGValue)(const GValue *),
    void (*takeIntoGValue)(GValue *, nativeType) = dummyTake<nativeType>,
    void (*setStaticInGValue)(GValue *, constNativeType) = dummySetStatic<constNativeType>
> class GValueTypedCXX : public GValueCXX
{
 public:
 typedef GValueTypedCXX<nativeType, constNativeType, gType, setGValue, getFromGValue, takeIntoGValue, setStaticInGValue> value_type;

    /**
     * prepare value, without setting it (isSet() will return false)
     */
    GValueTypedCXX() {
        init(gType);
    }
    /**
     * copy other value
     */
    GValueTypedCXX(const value_type &other) {
        init(gType);
        *this = other;
    }

    /**
     * copy value
     */
    GValueTypedCXX(constNativeType value) {
        init(gType);
        set(value);
    }

    /** copy other value */
    GValueCXX &operator = (const value_type &other) {
        if (&other != this) {
            g_value_copy(&other, this);
        }
        return *this;
    }

    /** copy other value */
    GValueCXX &operator = (constNativeType other) {
        set(other);
        return *this;
    }

    /**
     * set value, copying (string) or referencing (GObject) it if necessary
     */
    void set(constNativeType other) {
        setGValue(this, other);
    }

    /**
     * store pointer to static instance which does not have to be copied or freed
     * (like a const char * string)
     */
    void setStatic(constNativeType other) {
        setStaticInGValue(this, other);
    }

    /** transfer ownership of complex object (string, GObject) to GValue */
    void take(nativeType other) {
        takeIntoGValue(this, other);
    }

    /** access content without transfering ownership */
    constNativeType get() const {
        return getFromGValue(this);
    }

};

/**
 * Declares a C++ wrapper for a GValue containing a dynamically
 * created GType. Uses g_value_set/get/take_boxed() with the necessary
 * type casting.
 *
 * Example:
 * typedef GValueDynTypedCXX<GDateTime *, g_date_time_get_type> GValueDateTimeCXX;
 */
template<
    class nativeType,
    GType (*gTypeFactory)()
> class GValueDynTypedCXX : public GValueCXX
{
 public:
 typedef GValueDynTypedCXX<nativeType, gTypeFactory> value_type;

    /**
     * prepare value, without setting it (isSet() will return false)
     */
    GValueDynTypedCXX() {
        init(gTypeFactory());
    }
    /**
     * copy other value
     */
    GValueDynTypedCXX(const value_type &other) {
        init(gTypeFactory());
        *this = other;
    }

    /**
     * copy value
     */
    GValueDynTypedCXX(const nativeType value) {
        init(gTypeFactory());
        set(value);
    }

    /** copy other value */
    value_type &operator = (const value_type &other) {
        if (&other != this) {
            g_value_copy(&other, this);
        }
        return *this;
    }

    /** copy other value */
    value_type &operator = (const nativeType other) {
        set(other);
        return *this;
    }

    /**
     * set value, copying (string) or referencing (GObject) it if necessary
     */
    void set(const nativeType other) {
        g_value_set_boxed(this, other);
    }

    /**
     * store pointer to static instance which does not have to be copied or freed
     * (like a const char * string)
     */
    void setStatic(const nativeType other) {
        g_value_set_static_boxed(this, other);
    }

    /** transfer ownership of complex object (string, GObject) to GValue */
    void take(nativeType other) {
        g_value_take_boxed(this, other);
    }

    /** access content without transfering ownership */
    const nativeType get() const {
        return static_cast<const nativeType *>(g_value_get_boxed(this));
    }
};

typedef GValueTypedCXX<gboolean, gboolean, G_TYPE_BOOLEAN, g_value_set_boolean, g_value_get_boolean> GValueBooleanCXX;
typedef GValueTypedCXX<gint8, gint8, G_TYPE_CHAR, g_value_set_schar, g_value_get_schar> GValueCharCXX;
typedef GValueTypedCXX<guchar, guchar, G_TYPE_UCHAR, g_value_set_uchar, g_value_get_uchar> GValueUCharCXX;
typedef GValueTypedCXX<gint, gint, G_TYPE_INT, g_value_set_int, g_value_get_int> GValueIntCXX;
typedef GValueTypedCXX<guint, guint, G_TYPE_UINT, g_value_set_uint, g_value_get_uint> GValueUIntCXX;
typedef GValueTypedCXX<glong, glong, G_TYPE_LONG, g_value_set_long, g_value_get_long> GValueLongCXX;
typedef GValueTypedCXX<gulong, gulong, G_TYPE_ULONG, g_value_set_ulong, g_value_get_ulong> GValueULongCXX;
typedef GValueTypedCXX<gint64, gint64, G_TYPE_INT64, g_value_set_int64, g_value_get_int64> GValueInt64CXX;
typedef GValueTypedCXX<guint64, guint64, G_TYPE_UINT64, g_value_set_uint64, g_value_get_uint64> GValueUInt64CXX;
typedef GValueTypedCXX<gfloat, gfloat, G_TYPE_FLOAT, g_value_set_float, g_value_get_float> GValueFloatCXX;
typedef GValueTypedCXX<gdouble, gdouble, G_TYPE_DOUBLE, g_value_set_double, g_value_get_double> GValueDoubleCXX;
typedef GValueTypedCXX<gint, gint, G_TYPE_ENUM, g_value_set_enum, g_value_get_enum> GValueEnumCXX;
typedef GValueTypedCXX<gboolean, gboolean, G_TYPE_BOOLEAN, g_value_set_boolean, g_value_get_boolean> GValueBooleanCXX;
typedef GValueTypedCXX<gchar *, const gchar *, G_TYPE_STRING, g_value_set_string, g_value_get_string, g_value_take_string, g_value_set_static_string> GValueStringCXX;
typedef GValueTypedCXX<gpointer, gpointer, G_TYPE_OBJECT, g_value_set_object, g_value_get_object, g_value_take_object> GValueObjectCXX;



SE_END_CXX

#endif // INCL_GVALUE_SUPPORT

