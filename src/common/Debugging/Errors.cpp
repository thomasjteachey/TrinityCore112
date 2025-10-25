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
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <thread>

#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
#include <process.h>
#else
#include <unistd.h>
#if !defined(__EMSCRIPTEN__)
#include <execinfo.h>
#endif
#endif

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
#define Crash(message) \
    ULONG_PTR execeptionArgs[] = { reinterpret_cast<ULONG_PTR>(strdup(message)), reinterpret_cast<ULONG_PTR>(_ReturnAddress()) }; \
    RaiseException(EXCEPTION_ASSERTION_FAILURE, 0, 2, execeptionArgs);
#else
// should be easily accessible in gdb
extern "C" { TC_COMMON_API char const* TrinityAssertionFailedMessage = nullptr; }
#define Crash(message) \
    TrinityAssertionFailedMessage = strdup(message); \
    *((volatile int*)nullptr) = 0; \
    exit(1);
#endif

namespace
{
    std::string BuildCrashContext()
    {
        std::ostringstream context;

        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
        std::tm timeInfo;

#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
        localtime_s(&timeInfo, &nowTime);
#else
        localtime_r(&nowTime, &timeInfo);
#endif

        context << "Crash timestamp: " << std::put_time(&timeInfo, "%Y-%m-%d %H:%M:%S");

#if TRINITY_PLATFORM == TRINITY_PLATFORM_WINDOWS
        context << ", process id: " << _getpid();
#else
        context << ", process id: " << getpid();
#endif

        context << ", thread id: " << std::this_thread::get_id();

#if TRINITY_PLATFORM != TRINITY_PLATFORM_WINDOWS && !defined(__EMSCRIPTEN__)
        std::array<void*, 32> stack{};
        int frames = backtrace(stack.data(), stack.size());

        if (frames > 0)
        {
            char** symbols = backtrace_symbols(stack.data(), frames);

            if (symbols)
            {
                context << "\nBacktrace:";

                for (int i = 0; i < frames; ++i)
                    context << "\n  [" << i << "] " << symbols[i];

                free(symbols);
            }
        }
#endif

        return context.str();
    }

    void LogCrashMessage(std::string const& message)
    {
        if (message.empty())
            return;

        std::string const context = BuildCrashContext();

        if (!context.empty())
        {
            TC_LOG_FATAL("server.crash", "{}", context);
            fprintf(stderr, "%s\n", context.c_str());
            fflush(stderr);
        }

        TC_LOG_FATAL("server.crash", "{}", message);
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

} // namespace Trinity

std::string GetDebugInfo()
{
    return "";
}
