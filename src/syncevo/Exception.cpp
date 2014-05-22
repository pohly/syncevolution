/*
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2014 Intel Corporation
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
 */

#include <syncevo/Exception.h>
#include <src/syncevo/SynthesisEngine.h>
#include <synthesis/syerror.h>

#include "gdbus-cxx-bridge.h"

#include <pcrecpp.h>
#include <errno.h>

SE_BEGIN_CXX

static const char * const TRANSPORT_PROBLEM = "transport problem: ";
static const char * const SYNTHESIS_PROBLEM = "error code from Synthesis engine ";
static const char * const SYNCEVOLUTION_PROBLEM = "error code from SyncEvolution ";

SyncMLStatus Exception::handle(SyncMLStatus *status,
                               const std::string *logPrefix,
                               std::string *explanation,
                               Logger::Level level,
                               HandleExceptionFlags flags)
{
    // any problem here is a fatal local problem, unless set otherwise
    // by the specific exception
    SyncMLStatus new_status = SyncMLStatus(STATUS_FATAL + sysync::LOCAL_STATUS_CODE);
    std::string error;

    try {
        throw;
    } catch (const TransportException &ex) {
        SE_LOG_DEBUG(logPrefix, "TransportException thrown at %s:%d",
                     ex.m_file.c_str(), ex.m_line);
        error = std::string(TRANSPORT_PROBLEM) + ex.what();
        new_status = SyncMLStatus(sysync::LOCERR_TRANSPFAIL);
    } catch (const BadSynthesisResult &ex) {
        new_status = SyncMLStatus(ex.result());
        error = StringPrintf("%s%s",
                             SYNTHESIS_PROBLEM,
                             Status2String(new_status).c_str());
    } catch (const StatusException &ex) {
        new_status = ex.syncMLStatus();
        SE_LOG_DEBUG(logPrefix, "exception thrown at %s:%d",
                     ex.m_file.c_str(), ex.m_line);
        error = StringPrintf("%s%s: %s",
                             SYNCEVOLUTION_PROBLEM,
                             Status2String(new_status).c_str(), ex.what());
        if (new_status == STATUS_NOT_FOUND &&
            (flags & HANDLE_EXCEPTION_404_IS_OKAY)) {
            level = Logger::DEBUG;
        }
    } catch (const Exception &ex) {
        SE_LOG_DEBUG(logPrefix, "exception thrown at %s:%d",
                     ex.m_file.c_str(), ex.m_line);
        error = ex.what();
    } catch (const std::exception &ex) {
        error = ex.what();
    } catch (...) {
        error = "unknown error";
    }
    if (flags & HANDLE_EXCEPTION_FATAL) {
        level = Logger::ERROR;
    }
    if (flags & HANDLE_EXCEPTION_NO_ERROR) {
        level = Logger::DEBUG;
    }
    SE_LOG(logPrefix, level, "%s", error.c_str());
    if (flags & HANDLE_EXCEPTION_FATAL) {
        // Something unexpected went wrong, can only shut down.
        ::abort();
    }

    if (explanation) {
        *explanation = error;
    }

    if (status && *status == STATUS_OK) {
        *status = new_status;
    }
    return status ? *status : new_status;
}

void Exception::tryRethrow(const std::string &explanation, bool mustThrow)
{
    static const std::string statusre = ".* \\((?:local|remote), status (\\d+)\\)";
    int status;

    if (boost::starts_with(explanation, TRANSPORT_PROBLEM)) {
        SE_THROW_EXCEPTION(TransportException, explanation.substr(strlen(TRANSPORT_PROBLEM)));
    } else if (boost::starts_with(explanation, SYNTHESIS_PROBLEM)) {
        static const pcrecpp::RE re(statusre);
        if (re.FullMatch(explanation.substr(strlen(SYNTHESIS_PROBLEM)), &status)) {
            SE_THROW_EXCEPTION_1(BadSynthesisResult, "Synthesis engine failure", (sysync::TSyErrorEnum)status);
        }
    } else if (boost::starts_with(explanation, SYNCEVOLUTION_PROBLEM)) {
        static const pcrecpp::RE re(statusre + ": (.*)",
                                    pcrecpp::RE_Options().set_dotall(true));
        std::string details;
        if (re.FullMatch(explanation.substr(strlen(SYNCEVOLUTION_PROBLEM)), &status, &details)) {
            SE_THROW_EXCEPTION_STATUS(StatusException, details, (SyncMLStatus)status);
        }
    }

    if (mustThrow) {
        throw std::runtime_error(explanation);
    }
}

void Exception::tryRethrowDBus(const std::string &error)
{
    static const pcrecpp::RE re("(org\\.syncevolution(?:\\.\\w+)+): (.*)",
                                pcrecpp::RE_Options().set_dotall(true));
    std::string exName, explanation;
    if (re.FullMatch(error, &exName, &explanation)) {
        // found SyncEvolution exception explanation, parse it
        tryRethrow(explanation);
        // explanation not parsed, fall back to D-Bus exception
        throw GDBusCXX::dbus_error(exName, explanation);
    }
}

void Exception::throwError(const SourceLocation &where, const std::string &error)
{
    throwError(where, SyncMLStatus(STATUS_FATAL + sysync::LOCAL_STATUS_CODE), error);
}

void Exception::throwError(const SourceLocation &where, SyncMLStatus status, const std::string &error)
{
    throw StatusException(where.m_file, where.m_line, error, status);
}

void Exception::throwError(const SourceLocation &where, const std::string &action, int error)
{
    std::string what = action + ": " + strerror(error);
    // be as specific if we can be: relevant for the file backend,
    // which is expected to return STATUS_NOT_FOUND == 404 for "file
    // not found"
    if (error == ENOENT) {
        throwError(where, STATUS_NOT_FOUND, what);
    } else {
        throwError(where, what);
    }
}

void Exception::fatalError(void *object, const char *error)
{
    SE_LOG_ERROR(NULL, "%s", error);
    exit(1);
}

SE_END_CXX
