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
 * The boost::locale based implementation of locale-factory.h.
 */

#include "locale-factory.h"
#include "folks.h"

#include <boost/locale.hpp>

SE_BEGIN_CXX

class LocaleFactoryBoost : public LocaleFactory
{
    std::locale m_locale;

public:
    LocaleFactoryBoost() :
        m_locale(boost::locale::generator()(""))
    {}

    virtual boost::shared_ptr<IndividualCompare> createCompare(const std::string &order)
    {
        SE_THROW("not implemented");
    }

    virtual boost::shared_ptr<IndividualFilter> createFilter(const StringMap &filter)
    {
        SE_THROW("not implemented");
    }
};

boost::shared_ptr<LocaleFactory> LocaleFactory::createFactory()
{
    return boost::shared_ptr<LocaleFactory>(new LocaleFactoryBoost());
}

SE_END_CXX
