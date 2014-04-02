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

#ifndef INCL_SYNCEVOLUTION_EXCEPTION
# define INCL_SYNCEVOLUTION_EXCEPTION

#include <syncevo/Logging.h>
#include <syncevo/SyncML.h>

#include <string>

#include <syncevo/declarations.h>
SE_BEGIN_CXX

/** Encapsulates source information. */
class SourceLocation
{
 public:
    const char *m_file;
    int m_line;

    SourceLocation(const char *file, int line) :
       m_file(file),
       m_line(line)
    {}
};

/** Convenience macro to create a SourceLocation for the current location. */
#define SE_HERE SyncEvo::SourceLocation(__FILE__, __LINE__)

enum HandleExceptionFlags {
    HANDLE_EXCEPTION_FLAGS_NONE = 0,

    /**
     * a 404 status error is possible and must not be logged as ERROR
     */
    HANDLE_EXCEPTION_404_IS_OKAY = 1 << 0,
    HANDLE_EXCEPTION_FATAL = 1 << 1,
    /**
     * don't log exception as ERROR
     */
    HANDLE_EXCEPTION_NO_ERROR = 1 << 2,
    HANDLE_EXCEPTION_MAX = 1 << 3,
};

/**
 * an exception which records the source file and line
 * where it was thrown
 *
 * @TODO add function name
 */
class Exception : public std::runtime_error
{
 public:
    Exception(const std::string &file,
              int line,
              const std::string &what) :
    std::runtime_error(what),
        m_file(file),
        m_line(line)
        {}
    ~Exception() throw() {}
    const std::string m_file;
    const int m_line;

    /**
     * Convenience function, to be called inside a catch(..) block.
     *
     * Rethrows the exception to determine what it is, then logs it
     * at the chosen level (error by default).
     *
     * Turns certain known exceptions into the corresponding
     * status code if status still was STATUS_OK when called.
     * Returns updated status code.
     *
     * @param logPrefix      passed to SE_LOG* messages
     * @retval explanation   set to explanation for problem, if non-NULL
     * @param level     level to be used for logging
     */
    static SyncMLStatus handle(SyncMLStatus *status = NULL, const std::string *logPrefix = NULL, std::string *explanation = NULL, Logger::Level = Logger::ERROR, HandleExceptionFlags flags = HANDLE_EXCEPTION_FLAGS_NONE);
    static SyncMLStatus handle(const std::string &logPrefix, HandleExceptionFlags flags = HANDLE_EXCEPTION_FLAGS_NONE) { return handle(NULL, &logPrefix, NULL, Logger::ERROR, flags); }
    static SyncMLStatus handle(std::string &explanation, HandleExceptionFlags flags = HANDLE_EXCEPTION_FLAGS_NONE) { return handle(NULL, NULL, &explanation, Logger::ERROR, flags); }
    static void handle(HandleExceptionFlags flags) { handle(NULL, NULL, NULL, Logger::ERROR, flags); }
    static void log() { handle(NULL, NULL, NULL, Logger::DEBUG); }

    /**
     * Tries to identify exception class based on explanation string created by
     * handle(). If successful, that exception is throw with the same
     * attributes as in the original exception.
     *
     * If not, tryRethrow() returns (mustThrow false) or throws a std::runtime_error
     * with the explanation as text.
     */
    static void tryRethrow(const std::string &explanation, bool mustThrow = false);

    /**
     * Same as tryRethrow() for strings with a 'org.syncevolution.xxxx:' prefix,
     * as passed as D-Bus error strings.
     */
    static void tryRethrowDBus(const std::string &error);

    /**
     * throws a StatusException with a local, fatal error with the given string
     * or (on the iPhone, where exception handling is not
     * supported by the toolchain) prints an error directly
     * and aborts
     *
     * output format: <error>
     *
     * @param error     a string describing the error
     */
    static void throwError(const SourceLocation &where, const std::string &error) SE_NORETURN;

    /**
     * throw an exception with a specific status code after an operation failed and
     * remember that this instance has failed
     *
     * output format: <failure>
     *
     * @param status     a more specific status code; other throwError() variants
     *                   use STATUS_FATAL + sysync::LOCAL_STATUS_CODE, which is interpreted
     *                   as a fatal local error
     * @param action     a string describing what was attempted *and* how it failed
     */
    static void throwError(const SourceLocation &where, SyncMLStatus status, const std::string &failure) SE_NORETURN;

    /**
     * throw an exception after an operation failed and
     * remember that this instance has failed
     *
     * output format: <action>: <error string>
     *
     * @Param action   a string describing the operation or object involved
     * @param error    the errno error code for the failure
     */
    static void throwError(const SourceLocation &where, const std::string &action, int error) SE_NORETURN;

    /**
     * An error handler which prints the error message and then
     * stops the program. Never returns.
     *
     * The API was chosen so that it can be used as libebook/libecal
     * "backend-dies" signal handler.
     */
    static void fatalError(void *object, const char *error) SE_NORETURN;
};

/**
 * StatusException by wrapping a SyncML status
 */
class StatusException : public Exception
{
public:
    StatusException(const std::string &file,
                    int line,
                    const std::string &what,
                    SyncMLStatus status)
        : Exception(file, line, what), m_status(status)
    {}

    SyncMLStatus syncMLStatus() const { return m_status; }
protected:
    SyncMLStatus m_status;
};

class TransportException : public Exception
{
 public:
    TransportException(const std::string &file,
                       int line,
                       const std::string &what) :
    Exception(file, line, what) {}
    ~TransportException() throw() {}
};

class TransportStatusException : public StatusException
{
 public:
    TransportStatusException(const std::string &file,
                             int line,
                             const std::string &what,
                             SyncMLStatus status) :
    StatusException(file, line, what, status) {}
    ~TransportStatusException() throw() {}
};

/** throw a normal SyncEvolution Exception, including source information */
#define SE_THROW(_what) \
    SE_THROW_EXCEPTION(Exception, _what)

/** throw a class which accepts file, line, what parameters */
#define SE_THROW_EXCEPTION(_class,  _what) \
    throw _class(__FILE__, __LINE__, _what)

/** throw a class which accepts file, line, what plus 1 additional parameter */
#define SE_THROW_EXCEPTION_1(_class,  _what, _x1)   \
    throw _class(__FILE__, __LINE__, (_what), (_x1))

/** throw a class which accepts file, line, what plus 2 additional parameters */
#define SE_THROW_EXCEPTION_2(_class,  _what, _x1, _x2) \
    throw _class(__FILE__, __LINE__, (_what), (_x1), (_x2))

/** throw a class which accepts file, line, what plus 2 additional parameters */
#define SE_THROW_EXCEPTION_3(_class,  _what, _x1, _x2, _x3) \
    throw _class(__FILE__, __LINE__, (_what), (_x1), (_x2), (_x3))

/** throw a class which accepts file, line, what plus 2 additional parameters */
#define SE_THROW_EXCEPTION_4(_class,  _what, _x1, _x2, _x3, _x4) \
    throw _class(__FILE__, __LINE__, (_what), (_x1), (_x2), (_x3), (_x4))

/** throw a class which accepts file, line, what parameters and status parameters*/
#define SE_THROW_EXCEPTION_STATUS(_class,  _what, _status) \
    throw _class(__FILE__, __LINE__, _what, _status)

SE_END_CXX
#endif // INCL_SYNCEVOLUTION_EXCEPTION
