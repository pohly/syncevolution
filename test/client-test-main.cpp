/*
 * Copyright (C) 2008 Funambol, Inc.
 * Copyright (C) 2008-2009 Patrick Ohly <patrick.ohly@gmx.de>
 * Copyright (C) 2009 Intel Corporation
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

/** @cond API */
/** @addtogroup ClientTest */
/** @{ */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "test.h"

#include <signal.h>
#ifdef HAVE_VALGRIND_VALGRIND_H
# include <valgrind/valgrind.h>
# include <valgrind/memcheck.h>
#endif
#ifdef HAVE_EXECINFO_H
# include <execinfo.h>
#endif

#include <cppunit/CompilerOutputter.h>
#include <cppunit/ui/text/TestRunner.h>
#include <cppunit/TestListener.h>
#include <cppunit/TestResult.h>
#include <cppunit/TestFailure.h>
#include <cppunit/TestResultCollector.h>
#include <cppunit/extensions/TestFactoryRegistry.h>
#include <cppunit/extensions/HelperMacros.h>

#include <Logging.h>
#include <LogStdout.h>
#include <syncevo/LogRedirect.h>
#include <syncevo/SyncContext.h>
#include "ClientTest.h"

#include <boost/algorithm/string/split.hpp>

#include <pcrecpp.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef HAVE_SIGNAL_H
# include <signal.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <string>
#include <stdexcept>

#include <syncevo/declarations.h>
SE_BEGIN_CXX
using namespace std;

void simplifyFilename(string &filename)
{
    size_t pos = 0;
    while (true) {
        pos = filename.find(":", pos);
        if (pos == filename.npos ) {
            break;
        }
        filename.replace(pos, 1, "_");
    }
    pos = 0;
    while (true) {
        pos = filename.find("__", pos);
        if (pos == filename.npos) {
            break;
        }
        filename.erase(pos, 1);
    }
}

class ClientOutputter : public CppUnit::CompilerOutputter {
public:
    ClientOutputter(CppUnit::TestResultCollector *result, std::ostream &stream) :
        CompilerOutputter(result, stream) {}
    void write() {
        // Suppress writing useless test summary. We run only one test per process,
        // so this Outputter would not show the overall results.
        // CompilerOutputter::write();
    }
};

class ClientListener : public CppUnit::TestListener {
public:
    ClientListener() :
        m_failed(false),
        // Not really necessary, will be initialized once the test starts.
        // Set it anyway, to keep cppcheck happy.
        m_testFailed(false)
    {
#ifdef HAVE_SIGNAL_H
        // install signal handler which turns an alarm signal into a runtime exception
        // to abort tests which run too long
        const char *alarm = getenv("CLIENT_TEST_ALARM");
        m_alarmSeconds = alarm ? atoi(alarm) : -1;

        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = alarmTriggered;
        action.sa_flags = SA_NOMASK;
        sigaction(SIGALRM, &action, NULL);
#endif
    }

    ~ClientListener() {
        m_logger.reset();
    }

    void addAllowedFailures(string allowedFailures) {
        boost::split(m_allowedFailures, allowedFailures, boost::is_from_range(',', ','));
    }

    void startTest (CppUnit::Test *test) {
        m_currentTest = test->getName();
        std::cout << m_currentTest << std::flush;
        if (!getenv("SYNCEVOLUTION_DEBUG")) {
            string logfile = m_currentTest + ".log";
            simplifyFilename(logfile);
            m_logger.reset(new LogRedirect(LogRedirect::STDERR_AND_STDOUT, logfile.c_str()));
            m_logger->setLevel(Logger::DEBUG);
        }
        SE_LOG_DEBUG(NULL, "*** starting %s ***", m_currentTest.c_str());
        m_failures.reset();
        m_testFailed = false;

#ifdef HAVE_SIGNAL_H
        if (m_alarmSeconds > 0) {
            alarm(m_alarmSeconds);
        }
#endif
    }

    void addFailure(const CppUnit::TestFailure &failure) {
        m_failures.addFailure(failure);
        m_testFailed = true;
    }

    void endTest (CppUnit::Test *test) {
#ifdef HAVE_SIGNAL_H
        if (m_alarmSeconds > 0) {
            alarm(0);
        }
#endif

        std::string result;
        std::string failure;
        if (m_testFailed) {
            stringstream output;
            CppUnit::CompilerOutputter formatter(&m_failures, output);
            formatter.printFailureReport();
            failure = output.str();
            bool failed = true;
            for (const std::string &re: m_allowedFailures) {
                if (pcrecpp::RE(re).FullMatch(m_currentTest)) {
                    result = "*** failure ignored ***";
                    failed = false;
                    break;
                }
            }
            if (failed) {
                result = "*** failed ***";
                m_failed = true;
            }
        } else {
            result = "okay";
        }

        SE_LOG_DEBUG(NULL, "*** ending %s: %s ***", m_currentTest.c_str(), result.c_str());
        if (!failure.empty()) {
            SE_LOG_ERROR(NULL, "%s", failure.c_str());
        }
        m_logger.reset();

        string logfile = m_currentTest + ".log";
        simplifyFilename(logfile);
        
        const char* compareLog = getenv("CLIENT_TEST_COMPARE_LOG");
        if(compareLog && strlen(compareLog)) {
            int fd = open("____compare.log", O_RDONLY);
            if (fd >= 0) {
                int out = open(logfile.c_str(), O_WRONLY|O_APPEND);
                if (out >= 0) {
                    char buffer[4096];
                    bool cont = true;
                    ssize_t len;
                    while (cont && (len = read(fd, buffer, sizeof(buffer))) > 0) {
                        ssize_t total = 0;
                        while (cont && total < len) {
                            ssize_t written = write(out, buffer, len);
                            if (written < 0) {
                                perror(("writing " + logfile).c_str());
                                cont = false;
                            } else {
                                total += written;
                            }
                        }
                    }
                    if (len < 0) {
                        perror("reading ____compare.log");
                    }
                    close(out);
                }
                close(fd);
            }
        }

        std::cout << " " << result << "\n";
        if (!failure.empty()) {
            std::cout << failure << "\n";
        }
        std::cout << std::flush;
    }

    bool hasFailed() { return m_failed; }
    const string &getCurrentTest() const { return m_currentTest; }

private:
    set<string> m_allowedFailures;
    bool m_failed, m_testFailed;
    string m_currentTest;
#ifdef HAVE_SIGNAL_H
    int m_alarmSeconds;
#endif
    PushLogger<Logger> m_logger;
    CppUnit::TestResultCollector m_failures;

    static void alarmTriggered(int signal) {
        CPPUNIT_ASSERT_MESSAGE("test timed out", false);
    }
} syncListener;

const string &getCurrentTest() {
    return syncListener.getCurrentTest();
}

static void printTests(CppUnit::Test *test, int indention)
{
    if (!test) {
        return;
    }

    std::string name = test->getName();
    printf("%*s%s\n", indention * 3, "", name.c_str());
    for (int i = 0; i < test->getChildTestCount(); i++) {
        printTests(test->getChildTestAt(i), indention+1);
    }
}

static void addEnabledTests(CppUnit::Test *test, bool parentEnabled,
                            char **beginEnabled, char **endEnabled,
                            std::list<std::string> &result)
{
    if (!test) {
        return;
    }

    std::string name = test->getName();
    bool enabled = false;
    if (parentEnabled) {
        enabled = true;
    } else {
        for (char **selected = beginEnabled;
             selected < endEnabled;
             selected++) {
            if (name == *selected) {
                enabled = true;
                break;
            }
        }
    }

    if (dynamic_cast<CppUnit::TestLeaf *>(test)) {
        if (enabled) {
            result.push_back(name);
        }
    } else {
        for (int i = 0; i < test->getChildTestCount(); i++) {
            addEnabledTests(test->getChildTestAt(i), enabled,
                            beginEnabled, endEnabled,
                            result);
        }
    }
}

static void handler(int sig)
{
    void *buffer[100];
    int size;

    fprintf(stderr, "client-test %ld: \ncaught signal %d\n", (long)getpid(), sig);
    fflush(stderr);
#ifdef HAVE_EXECINFO_H
    size = backtrace(buffer, sizeof(buffer)/sizeof(buffer[0]));
    backtrace_symbols_fd(buffer, size, 2);
#endif
#ifdef HAVE_VALGRIND_VALGRIND_H
    VALGRIND_PRINTF_BACKTRACE("\ncaught signal %d\n", sig);
#endif
    /* system("objdump -l -C -d client-test >&2"); */
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = SIG_DFL;
    sigaction(SIGABRT, &act, NULL);
    abort();
}

extern "C"
int main(int argc, char* argv[])
{
  SyncContext::initMain("client-test");

  struct sigaction act;

  memset(&act, 0, sizeof(act));
  act.sa_handler = handler;
  sigaction(SIGABRT, &act, NULL);
  sigaction(SIGSEGV, &act, NULL);
  sigaction(SIGILL, &act, NULL);

  // Get the top level suite from the registry
  CppUnit::Test *suite = CppUnit::TestFactoryRegistry::getRegistry().makeTest();

  if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
      printf("usage: %s [test name]+\n\n"
             "Without arguments all available tests are run.\n"
             "Otherwise only the tests or group of tests listed are run.\n"
             "Here is the test hierarchy of this test program:\n",
             argv[0]);
      printTests(suite, 1);
      return 0;
  }

  // Adds the test to the list of test to run
  CppUnit::TextUi::TestRunner runner;
  runner.addTest( suite );

  // Change the default outputter to a compiler error format outputter
  runner.setOutputter( new ClientOutputter( &runner.result(),
                                            std::cout ) );

  // track current test and failure state
  const char *allowedFailures = getenv("CLIENT_TEST_FAILURES");
  if (allowedFailures) {
      syncListener.addAllowedFailures(allowedFailures);
  }
  runner.eventManager().addListener(&syncListener);


  if (getenv("SYNCEVOLUTION_DEBUG")) {
      Logger::instance().setLevel(Logger::DEBUG);
  }

  try {
      // Find all enabled tests.
      std::list<std::string> tests;
      if (argc <= 1) {
          // All tests.
          addEnabledTests(suite, true, NULL, NULL, tests);
      } else {
          // Some selected tests.
          addEnabledTests(suite, false, argv + 1, argv + argc, tests);
      }

      bool failed = false;
      if (tests.size() == 1) {
          // If one test, run it ourselves.
          runner.run(tests.front(), false, true, false);
          failed = syncListener.hasFailed();
      } else {
          // Otherwise act as test runner which invokes itself
          // recursively for each test. This way we keep running
          // even if one test crashes hard, valgrind can check
          // each test individually and uses less memory.
          for (const std::string &name: tests) {
#ifdef USE_SYSTEM
              if (system(StringPrintf("%s %s", argv[0], name.c_str()).c_str())) {
                  failed = true;
              }
#else
              // Avoid the intermediate shell process that system() uses and
              // do better result reporting.
              pid_t child = fork();
              if (child > 0) {
                  while (true) {
                      int status;
                      pid_t completed = waitpid(child, &status, 0);
                      if (completed == -1) {
                          perror("waitpid");
                          failed = true;
                      } else {
                          if (WIFEXITED(status)) {
                              int retcode = WEXITSTATUS(status);
                              if (retcode != 0) {
                                  printf("%s (%ld): failed with return code %d", name.c_str(), (long)child, retcode);
                                  failed = true;
                              }
                          } else if (WIFSIGNALED(status)) {
                              printf("%s (%ld): killed by signal %d", name.c_str(), (long)child, WTERMSIG(status));
                              failed = true;
                          }
                          fflush(stdout);
                          break;
                      }
                  }
              } else if (child == -1) {
                  perror("fork");
              } else {
                  // Use the test name also as name of the process.
                  execlp(argv[0], name.c_str(), name.c_str(), (char *)NULL);
                  perror("execlp");
                  _exit(1);
              }
#endif
          }
      }

      // Return error code 1 if the one of test failed.
      if (tests.size() > 1) {
          printf("%s", failed ? "FAILED" : "OK");
      }
      ClientTest::shutdown();
      return failed;
  } catch (invalid_argument e) {
      // Test path not resolved
      std::cout << std::endl
                << "ERROR: " << e.what()
                << std::endl;

      ClientTest::shutdown();
      return 1;
  }
}

/** @} */
/** @endcond */

SE_END_CXX
