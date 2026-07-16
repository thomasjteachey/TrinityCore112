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
#include <cerrno>
#include <fcntl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
// should be easily accessible in gdb
extern "C" { TC_COMMON_API char const* TrinityAssertionFailedMessage = nullptr; }
#endif

namespace
{
    thread_local std::string ThreadCrashContext;
#if TRINITY_PLATFORM != TRINITY_PLATFORM_WINDOWS
    constexpr std::size_t EmergencyCrashContextSize = 2048;
    thread_local char EmergencyCrashContext[EmergencyCrashContextSize] = {};
    volatile sig_atomic_t EmergencyCrashFileDescriptor = -1;

    void EmergencyWriteToFd(int fd, char const* data, std::size_t length)
    {
        if (fd < 0 || !data || !length)
            return;

        while (length)
        {
            ssize_t written = write(fd, data, length);
            if (written > 0)
            {
                data += written;
                length -= std::size_t(written);
                continue;
            }

            if (written < 0 && errno == EINTR)
                continue;

            break;
        }
    }

    void EmergencyWrite(char const* data, std::size_t length)
    {
        // The crash file is the durable destination. Write it before stderr:
        // stderr may be a full systemd/journald pipe during a high-load crash,
        // and blocking there must not prevent Crash.log from receiving bytes.
        int fd = EmergencyCrashFileDescriptor;
        if (fd >= 0 && fd != STDERR_FILENO)
            EmergencyWriteToFd(fd, data, length);

        EmergencyWriteToFd(STDERR_FILENO, data, length);
    }

    std::size_t EmergencyStringLength(char const* text, std::size_t maximum = static_cast<std::size_t>(-1))
    {
        if (!text)
            return 0;

        std::size_t length = 0;
        while (length < maximum && text[length])
            ++length;
        return length;
    }

    void EmergencyWriteLiteral(char const* text)
    {
        EmergencyWrite(text, EmergencyStringLength(text));
    }

    char* AppendUnsignedDecimal(char* output, uint64 value)
    {
        char reversed[32];
        std::size_t count = 0;
        do
        {
            reversed[count++] = char('0' + (value % 10));
            value /= 10;
        } while (value && count < sizeof(reversed));

        while (count)
            *output++ = reversed[--count];
        return output;
    }

    char* AppendHexPointer(char* output, uintptr_t value)
    {
        static char constexpr HexDigits[] = "0123456789abcdef";
        *output++ = '0';
        *output++ = 'x';

        bool started = false;
        for (int shift = int(sizeof(uintptr_t) * 8) - 4; shift >= 0; shift -= 4)
        {
            uint8 digit = uint8((value >> shift) & 0xF);
            if (digit || started || shift == 0)
            {
                *output++ = HexDigits[digit];
                started = true;
            }
        }
        return output;
    }

    char const* GetFatalSignalName(int signalId)
    {
        switch (signalId)
        {
            case SIGSEGV: return "Segmentation fault";
            case SIGFPE:  return "Floating point exception";
            case SIGILL:  return "Illegal instruction";
#ifdef SIGBUS
            case SIGBUS:  return "Bus error";
#endif
            case SIGABRT: return "Aborted";
            default:      return "Unknown";
        }
    }

    void EmergencyLogFatalSignalHeader(int signalId, siginfo_t const* signalInfo)
    {
        // Keep the first durable write small and independent of stack unwinding,
        // allocation, the normal logger, and thread-local crash context.
        char line[512];
        char* cursor = line;
        char const prefix[] = "\nCaught fatal signal ";
        for (char character : prefix)
            if (character)
                *cursor++ = character;
        cursor = AppendUnsignedDecimal(cursor, uint64(signalId));
        *cursor++ = ' ';
        *cursor++ = '(';
        char const* signalName = GetFatalSignalName(signalId);
        std::size_t signalNameLength = EmergencyStringLength(signalName);
        for (std::size_t i = 0; i < signalNameLength; ++i)
            *cursor++ = signalName[i];
        *cursor++ = ')';
        *cursor++ = '\n';

        char const processPrefix[] = "Process id: ";
        for (char character : processPrefix)
            if (character)
                *cursor++ = character;
        cursor = AppendUnsignedDecimal(cursor, uint64(getpid()));
        *cursor++ = '\n';

#ifdef SYS_gettid
        char const threadPrefix[] = "Thread id: ";
        for (char character : threadPrefix)
            if (character)
                *cursor++ = character;
        cursor = AppendUnsignedDecimal(cursor, uint64(syscall(SYS_gettid)));
        *cursor++ = '\n';
#endif

        if (signalInfo)
        {
            char const codePrefix[] = "Signal code: ";
            for (char character : codePrefix)
                if (character)
                    *cursor++ = character;
            if (signalInfo->si_code < 0)
            {
                *cursor++ = '-';
                cursor = AppendUnsignedDecimal(cursor, uint64(-int64(signalInfo->si_code)));
            }
            else
                cursor = AppendUnsignedDecimal(cursor, uint64(signalInfo->si_code));
            *cursor++ = '\n';

            if (signalId == SIGSEGV
#ifdef SIGBUS
                || signalId == SIGBUS
#endif
            )
            {
                char const addressPrefix[] = "Fault address: ";
                for (char character : addressPrefix)
                    if (character)
                        *cursor++ = character;
                cursor = AppendHexPointer(cursor, reinterpret_cast<uintptr_t>(signalInfo->si_addr));
                *cursor++ = '\n';
            }
        }

        EmergencyWrite(line, std::size_t(cursor - line));
    }

    void EmergencyLogFatalSignalDetails()
    {
        char line[256];
        char* cursor = line;

        if (EmergencyCrashContext[0])
        {
            EmergencyWriteLiteral("Last crash context:\n  ");
            EmergencyWrite(EmergencyCrashContext, EmergencyStringLength(EmergencyCrashContext, EmergencyCrashContextSize));
            EmergencyWriteLiteral("\n");
        }

        constexpr int32 MaxFrames = 64;
        void* stack[MaxFrames];
        int32 capturedFrames = backtrace(stack, MaxFrames);
        if (capturedFrames > 0)
        {
            EmergencyWriteLiteral("Raw stack addresses:\n");
            for (int32 i = 0; i < capturedFrames; ++i)
            {
                cursor = line;
                *cursor++ = ' ';
                *cursor++ = ' ';
                *cursor++ = '[';
                cursor = AppendUnsignedDecimal(cursor, uint64(i));
                *cursor++ = ']';
                *cursor++ = ' ';
                cursor = AppendHexPointer(cursor, reinterpret_cast<uintptr_t>(stack[i]));
                *cursor++ = '\n';
                EmergencyWrite(line, std::size_t(cursor - line));
            }
        }

        EmergencyWriteLiteral("End emergency crash report.\n");
    }
#endif

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

    thread_local volatile sig_atomic_t HandlingFatalSignal = 0;
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
#if TRINITY_PLATFORM != TRINITY_PLATFORM_WINDOWS
    std::size_t length = ThreadCrashContext.size();
    if (length >= EmergencyCrashContextSize)
        length = EmergencyCrashContextSize - 1;
    std::memcpy(EmergencyCrashContext, ThreadCrashContext.data(), length);
    EmergencyCrashContext[length] = '\0';
#endif
}

void ClearCrashContext()
{
    ThreadCrashContext.clear();
#if TRINITY_PLATFORM != TRINITY_PLATFORM_WINDOWS
    EmergencyCrashContext[0] = '\0';
#endif
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
namespace
{
    void FatalSignalHandlerWithInfo(int sigval, siginfo_t* signalInfo, void*)
    {
        // This must be the first operation: commit a minimal report to Crash.log
        // before touching TLS, unwinding, the normal logger, or stderr.
        EmergencyLogFatalSignalHeader(sigval, signalInfo);

        if (HandlingFatalSignal)
        {
            EmergencyWriteLiteral("Recursive fatal signal while writing crash report.\n");
            _exit(128 + sigval);
        }

        HandlingFatalSignal = 1;
        EmergencyLogFatalSignalDetails();

        // Restore the default disposition only after the durable report exists,
        // then re-raise so systemd/core_pattern/gdb still receive the original
        // fatal signal and generate a normal core dump.
        struct sigaction defaultAction;
        memset(&defaultAction, 0, sizeof(defaultAction));
        defaultAction.sa_handler = SIG_DFL;
        sigemptyset(&defaultAction.sa_mask);
        sigaction(sigval, &defaultAction, nullptr);

        sigset_t unblockedSignals;
        sigemptyset(&unblockedSignals);
        sigaddset(&unblockedSignals, sigval);
        sigprocmask(SIG_UNBLOCK, &unblockedSignals, nullptr);

#ifdef SYS_tgkill
        syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), sigval);
#else
        raise(sigval);
#endif
        _exit(128 + sigval);
    }
}

void FatalSignalHandler(int sigval)
{
    FatalSignalHandlerWithInfo(sigval, nullptr, nullptr);
}

void InitializeEmergencyCrashLog(std::string const& filename)
{
    if (filename.empty())
        return;

    int newFd = open(filename.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (newFd < 0)
    {
        fprintf(stderr, "Could not open emergency crash log '%s': %s\n", filename.c_str(), std::strerror(errno));
        return;
    }

    int oldFd = EmergencyCrashFileDescriptor;
    EmergencyCrashFileDescriptor = newFd;
    if (oldFd >= 0)
        close(oldFd);

    // Warm up libgcc's unwinder before a low-memory or corrupted-state crash.
    void* warmupFrame[1];
    backtrace(warmupFrame, 1);
}

void InitCurrentThreadCrashSignalStack()
{
    // Alternate signal stacks are a property of the calling thread, not of
    // the process. A stack installed by the main thread does not help a map,
    // database, network, CLI, or SOAP worker whose regular stack overflowed.
    alignas(16) thread_local unsigned char alternateSignalStack[64 * 1024];
    thread_local bool alternateSignalStackInstalled = false;
    if (alternateSignalStackInstalled)
        return;

    stack_t currentStack = {};
    if (sigaltstack(nullptr, &currentStack) == 0 && !(currentStack.ss_flags & SS_DISABLE))
    {
        // Respect an alternate stack installed by another subsystem.
        alternateSignalStackInstalled = true;
        return;
    }

    stack_t alternateStack = {};
    alternateStack.ss_sp = alternateSignalStack;
    alternateStack.ss_size = sizeof(alternateSignalStack);
    alternateStack.ss_flags = 0;
    if (sigaltstack(&alternateStack, nullptr) != 0)
    {
        fprintf(stderr, "Could not install alternate fatal-signal stack for current thread: %s\n", std::strerror(errno));
        return;
    }

    alternateSignalStackInstalled = true;
}

void InitCrashSignalHandlers()
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = &FatalSignalHandlerWithInfo;
    sigemptyset(&action.sa_mask);

    InitCurrentThreadCrashSignalStack();

    // Do not use SA_RESETHAND here. It resets the disposition process-wide
    // before the handler starts, so a near-simultaneous fault on another map
    // worker can terminate the process before the first worker writes anything.
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;

    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGFPE, &action, nullptr);
    sigaction(SIGILL, &action, nullptr);
#ifdef SIGBUS
    sigaction(SIGBUS, &action, nullptr);
#endif
    sigaction(SIGABRT, &action, nullptr);
}
#else
void InitCurrentThreadCrashSignalStack()
{
}
#endif

} // namespace Trinity

std::string GetDebugInfo()
{
    return "";
}
