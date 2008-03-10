/*
 * Copyright (C) 2008 Patrick Ohly
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "EvolutionCalendarSource.h"
#include "EvolutionMemoSource.h"

static EvolutionSyncSource *createSource(const EvolutionSyncSourceParams &params)
{
    pair <string, string> sourceType = EvolutionSyncSource::getSourceType(params.m_nodes);
    bool isMe;

    isMe = sourceType.first == "evolution-tasks";
    if (isMe || sourceType.first == "todo") {
        if (sourceType.second == "" || sourceType.second != "text/calendar") {
#ifdef ENABLE_ECAL
            return new EvolutionCalendarSource(E_CAL_SOURCE_TYPE_TODO, params);
#else
            return isMe ? RegisterSyncSource::InactiveSource : NULL;
#endif
        } else {
            return NULL;
        }
    }

    isMe = sourceType.first == "evolution-memo";
    if (isMe || sourceType.second == "memo") {
        if (sourceType.second == "" || sourceType.second == "text/plain") {
#ifdef ENABLE_ECAL
            return new EvolutionMemoSource(params);
#else
            return isMe ? RegisterSyncSource::InactiveSource : NULL;
#endif
        } else if (sourceType.second == "text/calendar") {
#ifdef ENABLE_ECAL
            return new EvolutionCalendarSource(E_CAL_SOURCE_TYPE_JOURNAL, params);
#else
            return isMe ? RegisterSyncSource::InactiveSource : NULL;
#endif
        } else {
            return NULL;
        }
    }

    isMe = sourceType.first == "evolution-calendar";
    if (isMe || sourceType.first == "calendar") {
        if (sourceType.second == "" || sourceType.second == "text/calendar") {
#ifdef ENABLE_ECAL
            return new EvolutionCalendarSource(E_CAL_SOURCE_TYPE_EVENT, params);
#else
            return isMe ? RegisterSyncSource::InactiveSource : NULL;
#endif
        } else {
            return NULL;
        }
    }

    return NULL;
}

static RegisterSyncSource registerMe("Evolution Calendar/Task Lists/Memos",
#ifdef ENABLE_ECAL
                                     true,
#else
                                     false,
#endif
                                     createSource,
                                     "Evolution Calendar = calendar = events = evolution-events\n"
                                     "   always uses iCalendar 2.0\n"
                                     "Evolution Task Lists = todo = tasks = evolution-tasks\n"
                                     "   always uses iCalendar 2.0\n"
                                     "Evolution Memos = memo = evolution-memo\n"
                                     "   plain text in UTF-8 (default) = text/plain\n"
                                     "   iCalendar 2.0 = text/calendar\n"
                                     "   The later format is not tested because none of the\n"
                                     "   supported SyncML servers accepts it.\n",
                                     Values() +
                                     (Aliases("evolution-calendar") + "Evolution Calendar") +
                                     (Aliases("evolution-tasks") + "Evolution Task Lists") +
                                     (Aliases("evolution-memo") + "Evolution Memos"));
