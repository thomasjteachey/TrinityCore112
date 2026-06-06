/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Errors.h"
#include "Log.h"
#include "StringFormat.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <thread>
#include <sstream>
#include <utility>

/**
    @file Errors.cpp

    @brief This file contains definitions of functions used for reporting critical application errors

    It is very important that (std::)abort is NEVER called in place of *((volatile int*)NULL) = 0;
    Calling abort() on Windows does not invoke unhandled exception filters - a mechanism used by WheatyExceptionReport
    to log crashes. exit(1) calls here are for static analysis tools to indicate that calling functions defined in this file
    terminates the application.
 */

#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
#include <Windows.h>
#else
#include <execinfo.h>
// should be easily accessible in gdb
extern "C" { TC_COMMON_API char const* TrinityAssertionFailedMessage = nullptr; }
#endif

namespace
{
    thread_local std::string ThreadCrashContext;

    void LogCrashMessage(std::string const& message)
    {
        if (message.empty())
            return;

        TC_LOG_FATAL("server.crash", "{}", message);
    }

    void LogCrashContext()
    {
        if (ThreadCrashContext.empty())
            return;

        std::string formattedMessage = "\nLast crash context:\n  " + ThreadCrashContext + '\n';
        LogCrashMessage(formattedMessage);
        fprintf(stderr, "%s", formattedMessage.c_str());
        fflush(stderr);
    }

#if TRINITY_PLATFORM != TRINITY_PLATFORM_WINDOWS
    void LogStackTrace()
    {
        constexpr int32 MaxFrames = 64;
        void* stack[MaxFrames];
        int32 capturedFrames = backtrace(stack, MaxFrames);
        if (capturedFrames <= 0)
            return;

        char** symbols = backtrace_symbols(stack, capturedFrames);
        if (!symbols)
            return;

        std::ostringstream stream;
        stream << "\nStack trace:\n";
        for (int32 i = 0; i < capturedFrames; ++i)
            stream << "  [" << i << "] " << symbols[i] << '\n';

        std::string trace = stream.str();
        LogCrashMessage(trace);
        fprintf(stderr, "%s", trace.c_str());
        fflush(stderr);

        free(symbols);
    }

    std::atomic_bool HandlingFatalSignal = false;

    void HandleFatalSignal(int signalId, char const* signalName)
    {
        std::string formattedMessage = Trinity::StringFormat("\nCaught fatal signal {} ({})\n", signalId, signalName);
        LogCrashMessage(formattedMessage);
        fprintf(stderr, "%s", formattedMessage.c_str());
        fflush(stderr);

        LogCrashContext();
        LogStackTrace();
    }
#endif

    [[noreturn]] void Crash(char const* message)
    {
#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
        ULONG_PTR execeptionArgs[] = { reinterpret_cast<ULONG_PTR>(strdup(message)), reinterpret_cast<ULONG_PTR>(_ReturnAddress()) };
        RaiseException(EXCEPTION_ASSERTION_FAILURE, 0, 2, execeptionArgs);
#else
        TrinityAssertionFailedMessage = strdup(message);
        LogCrashContext();
        LogStackTrace();
        *((volatile int*)nullptr) = 0;
        exit(1);
#endif
    }

    std::string FormatAssertionMessage(char const* format, va_list args)
    {
        std::string formatted;
        va_list len;

        va_copy(len, args);
        int32 length = vsnprintf(nullptr, 0, format, len);
        va_end(len);

        formatted.resize(length);
        vsnprintf(&formatted[0], length + 1, format, args);

        return formatted;
    }
}

namespace Trinity
{

void SetCrashContext(std::string context)
{
    ThreadCrashContext = std::move(context);
}

void ClearCrashContext()
{
    ThreadCrashContext.clear();
}

void Assert(char const* file, int line, char const* function, std::string debugInfo, char const* message)
{
    std::string formattedMessage = StringFormat("\n{}:{} in {} ASSERTION FAILED:\n  {}\n", file, line, function, message) + debugInfo + '\n';
    LogCrashMessage(formattedMessage);
    fprintf(stderr, "%s", formattedMessage.c_str());
    fflush(stderr);
    Crash(formattedMessage.c_str());
}

void Assert(char const* file, int line, char const* function, std::string debugInfo, char const* message, char const* format, ...)
{
    va_list args;
    va_start(args, format);

    std::string formattedMessage = StringFormat("\n{}:{} in {} ASSERTION FAILED:\n  {}\n", file, line, function, message) + FormatAssertionMessage(format, args) + '\n' + debugInfo + '\n';
    va_end(args);

    LogCrashMessage(formattedMessage);
    fprintf(stderr, "%s", formattedMessage.c_str());
    fflush(stderr);

    Crash(formattedMessage.c_str());
}

void Fatal(char const* file, int line, char const* function, char const* message, ...)
{
    va_list args;
    va_start(args, message);

    std::string formattedMessage = StringFormat("\n{}:{} in {} FATAL ERROR:\n", file, line, function) + FormatAssertionMessage(message, args) + '\n';
    va_end(args);

    LogCrashMessage(formattedMessage);
    fprintf(stderr, "%s", formattedMessage.c_str());
    fflush(stderr);

    std::this_thread::sleep_for(std::chrono::seconds(10));
    Crash(formattedMessage.c_str());
}

void Error(char const* file, int line, char const* function, char const* message)
{
    std::string formattedMessage = StringFormat("\n{}:{} in {} ERROR:\n  {}\n", file, line, function, message);
    LogCrashMessage(formattedMessage);
    fprintf(stderr, "%s", formattedMessage.c_str());
    fflush(stderr);
    Crash(formattedMessage.c_str());
}

void Warning(char const* file, int line, char const* function, char const* message)
{
    fprintf(stderr, "\n%s:%i in %s WARNING:\n  %s\n",
                   file, line, function, message);
}

void Abort(char const* file, int line, char const* function)
{
    std::string formattedMessage = StringFormat("\n{}:{} in {} ABORTED.\n", file, line, function);
    LogCrashMessage(formattedMessage);
    fprintf(stderr, "%s", formattedMessage.c_str());
    fflush(stderr);
    Crash(formattedMessage.c_str());
}

void Abort(char const* file, int line, char const* function, char const* message, ...)
{
    va_list args;
    va_start(args, message);

    std::string formattedMessage = StringFormat("\n{}:{} in {} ABORTED:\n", file, line, function) + FormatAssertionMessage(message, args) + '\n';
    va_end(args);

    LogCrashMessage(formattedMessage);
    fprintf(stderr, "%s", formattedMessage.c_str());
    fflush(stderr);

    Crash(formattedMessage.c_str());
}

void AbortHandler(int sigval)
{
    // nothing useful to log here, no way to pass args
    std::string formattedMessage = StringFormat("Caught signal {}\n", sigval);
    LogCrashMessage(formattedMessage);
    fprintf(stderr, "%s", formattedMessage.c_str());
    fflush(stderr);
    Crash(formattedMessage.c_str());
}

#if TRINITY_PLATFORM != TRINITY_PLATFORM_WINDOWS
void FatalSignalHandler(int sigval)
{
    // prevent recursive handling that might occur if the crash originates inside the handler itself
    if (HandlingFatalSignal.exchange(true))
        raise(sigval);

    char const* name = strsignal(sigval);
    HandleFatalSignal(sigval, name ? name : "unknown");

    // Restore default handler and re-raise so normal core dumps or debugger hooks still occur
    signal(sigval, SIG_DFL);
    raise(sigval);
}

void InitCrashSignalHandlers()
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = &FatalSignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESETHAND;

    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
#ifdef SIGBUS
    sigaction(SIGBUS, &action, nullptr);
#endif

    signal(SIGABRT, &AbortHandler);
}
#endif

} // namespace Trinity

std::string GetDebugInfo()
{
    return "";
}
