/*
 * A way to ask a running realm to shut itself down politely.
 *
 * Jenkins used to deploy by killing the process outright:
 *
 *     sudo systemctl stop  $SERVICE
 *     sudo cmake --install $BUILD_DIR
 *     sudo systemctl start $SERVICE
 *
 * Everybody online is dropped mid-swing with no warning and no idea why, and
 * the world is saved by whatever the shutdown path manages on its way out
 * rather than by a clean, announced stop.
 *
 * TrinityCore already knows how to do this properly - World::ShutdownServ
 * counts down, tells every client, refuses new logins near the end and saves
 * on the way out. The problem is purely that there is no way to ASK for it
 * from outside the game on this box: the console is not a tty under systemd,
 * SOAP is disabled, and enabling SOAP would mean a GM account whose password
 * has to live in the build job.
 *
 * So the request comes in as a file. A deploy writes one line of seconds and
 * a message, this notices within a few seconds, says the message to everybody
 * in their chat frame and hands the rest to ShutdownServ.
 *
 * The exit code is deliberately SHUTDOWN_EXIT_CODE (0). The unit is
 * Restart=on-failure, so a clean stop stays stopped and the deploy is free to
 * install over the binary - where a non-zero code would have systemd race the
 * installer by bringing the old build straight back up.
 *
 * The answer goes back out the same way, as Centurion.Shutdown.StatusFile:
 *
 *     state=scheduled|cancelled|none
 *     seconds=<time left when the timer was last set>
 *     ours=<1 if a deploy request set it, 0 if somebody in the game did>
 *     serial=<increments on every change>
 *
 * That exists so a deploy can tell "the realm is counting down as asked" from
 * "an operator has taken hold of the timer". `.server restart force 6000` runs
 * ShutdownServ again with a much larger value, which lands here as a second
 * initiate with ours=0; `.server shutdown cancel` lands as state=cancelled.
 * Without it the deploy could only wait a fixed margin and then kill the realm
 * anyway, which is exactly what holding the restart is meant to prevent.
 */

#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "World.h"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace
{
    std::string s_requestFile;
    std::string s_statusFile;
    uint32 s_pollMs = 3000;
    uint32 s_serial = 0;

    // Set immediately before this script's own ShutdownServ call and consumed by
    // the initiate hook, so the status can say who set the timer. Atomic with an
    // exchange rather than a plain bool, because ShutdownServ is also reachable
    // from a map thread through World::TriggerGuidWarning - exactly one caller
    // must be able to claim the flag.
    std::atomic<bool> s_requestInFlight{ false };

    void LoadShutdownConfig()
    {
        s_requestFile = sConfigMgr->GetStringDefault("Centurion.Shutdown.RequestFile", "");
        s_pollMs = uint32(std::max(500, sConfigMgr->GetIntDefault("Centurion.Shutdown.PollMs", 3000)));
        s_statusFile = sConfigMgr->GetStringDefault("Centurion.Shutdown.StatusFile", "");
        if (s_statusFile.empty() && !s_requestFile.empty())
            s_statusFile = s_requestFile + ".status";

        if (!s_requestFile.empty())
            TC_LOG_INFO("server.worldserver",
                "Graceful shutdown requests are read from '{}' every {}ms; state is published to '{}'.",
                s_requestFile, s_pollMs, s_statusFile);
    }

    // Written whole, then moved into place. A deploy script polls this to decide
    // whether it may stop the realm, so it must never be able to read a half
    // written file and act on the fragment.
    //
    // Locked because ShutdownServ is NOT reached only from the world thread:
    // ObjectGuid.cpp calls World::TriggerGuidWarning/TriggerGuidAlert when a guid
    // range runs low, and World.cpp's own comment there says the lock exists "to
    // prevent multiple maps triggering at the same time". So two map threads can
    // arrive here at once, and ++s_serial plus the rename must not interleave.
    std::mutex s_statusLock;

    void WriteStatus(char const* state, uint32 seconds, bool ours)
    {
        if (s_statusFile.empty())
            return;

        std::lock_guard<std::mutex> guard(s_statusLock);

        std::string const temp = s_statusFile + ".part";
        {
            std::ofstream out(temp.c_str(), std::ios::trunc);
            if (!out.is_open())
            {
                TC_LOG_ERROR("server.worldserver", "Cannot write shutdown status to '{}'.", temp);
                return;
            }

            out << "state=" << state << "\n"
                << "seconds=" << seconds << "\n"
                << "ours=" << (ours ? 1 : 0) << "\n"
                << "serial=" << (s_serial + 1) << "\n";

            out.flush();
            if (!out.good())
            {
                // Bail out with the PREVIOUS status still in place. A full disk
                // used to mean the good file was already deleted and a truncated
                // one published in its stead, which a deploy would then read.
                TC_LOG_ERROR("server.worldserver",
                    "Shutdown status '{}' did not write cleanly; leaving the previous one in place.", temp);
                out.close();
                std::remove(temp.c_str());
                return;
            }
        }

        // rename() replaces an existing file atomically on POSIX, which is what
        // this realm runs on - so the reader can never catch a moment where the
        // status is missing. The remove-then-rename fallback is only for platforms
        // that refuse to overwrite.
        if (std::rename(temp.c_str(), s_statusFile.c_str()) != 0)
        {
            std::remove(s_statusFile.c_str());
            if (std::rename(temp.c_str(), s_statusFile.c_str()) != 0)
            {
                TC_LOG_ERROR("server.worldserver",
                    "Cannot move shutdown status '{}' into place at '{}'.", temp, s_statusFile);
                return;
            }
        }

        ++s_serial;
    }

    // "<seconds>" on the first line, the rest of the file is the message. Two
    // pieces rather than a delimiter because a message is free text and any
    // separator picked here would eventually turn up inside one.
    bool ReadRequest(std::string const& path, uint32& outSeconds, std::string& outMessage)
    {
        std::ifstream in(path.c_str());
        if (!in.is_open())
            return false;

        std::string firstLine;
        if (!std::getline(in, firstLine))
            return false;

        try
        {
            outSeconds = uint32(std::max(0, std::stoi(firstLine)));
        }
        catch (std::exception const&)
        {
            TC_LOG_ERROR("server.worldserver",
                "Shutdown request '{}' does not start with a number of seconds; ignoring.", path);
            return false;
        }

        std::ostringstream rest;
        std::string line;
        while (std::getline(in, line))
            rest << (rest.tellp() ? "\n" : "") << line;

        outMessage = rest.str();
        return true;
    }

    class centurion_graceful_shutdown : public WorldScript
    {
    public:
        centurion_graceful_shutdown() : WorldScript("centurion_graceful_shutdown") { }

        void OnConfigLoad(bool /*reload*/) override { LoadShutdownConfig(); }

        // Clear state left behind by the previous boot.
        //
        // The request file matters more than the status here: the watcher deletes a
        // request as it honours it, but a realm that was hard-killed before it got
        // the chance leaves one on disk - and this realm would then read it seconds
        // after booting and shut itself straight back down. Anything still sitting
        // there at startup is stale by definition, because a deploy only ever
        // writes one to a realm that is already up.
        void OnStartup() override
        {
            if (!s_requestFile.empty() && std::remove(s_requestFile.c_str()) == 0)
                TC_LOG_INFO("server.worldserver",
                    "Discarded a shutdown request left over from the previous run ('{}').", s_requestFile);

            WriteStatus("none", 0, false);
        }

        // Every route into ShutdownServ arrives here, including `.server restart`
        // and `.server shutdown` typed by a GM in the game. Whoever set the timer
        // last owns it, and a deploy that did not set it must leave the realm be.
        void OnShutdownInitiate(ShutdownExitCode /*code*/, ShutdownMask /*mask*/) override
        {
            bool const ours = s_requestInFlight.exchange(false);

            uint32 const left = sWorld->GetShutDownTimeLeft();
            WriteStatus("scheduled", left, ours);

            if (!ours)
                TC_LOG_INFO("server.worldserver",
                    "Shutdown timer set to {}s from inside the game; a deploy waiting on this realm "
                    "will stand down rather than stop it.", left);
        }

        void OnShutdownCancel() override
        {
            s_requestInFlight = false;
            WriteStatus("cancelled", 0, false);
            TC_LOG_INFO("server.worldserver", "Shutdown cancelled; a deploy waiting on this realm will stand down.");
        }

        void OnUpdate(uint32 diff) override
        {
            if (s_requestFile.empty())
                return;

            _elapsed += diff;
            if (_elapsed < s_pollMs)
                return;
            _elapsed = 0;

            uint32 seconds = 0;
            std::string message;
            if (!ReadRequest(s_requestFile, seconds, message))
                return;

            // Removed BEFORE acting on it. A request that somehow cannot be
            // honoured must not be re-read every three seconds for the rest of
            // the uptime, and a countdown that is restarted on every poll never
            // reaches zero.
            std::remove(s_requestFile.c_str());

            TC_LOG_INFO("server.worldserver",
                "Graceful shutdown requested in {}s: {}", seconds,
                message.empty() ? "(no message)" : message.c_str());

            // The operator's words first, in everyone's chat frame. ShutdownServ
            // does its own countdown on top of this - it tells the CLIENT how
            // long is left, which is the part that draws the timer - so this
            // only has to carry the reason.
            if (!message.empty())
                sWorld->SendServerMessage(SERVER_MSG_STRING, message);

            s_requestInFlight = true;
            sWorld->ShutdownServ(seconds, 0, SHUTDOWN_EXIT_CODE, message);
            s_requestInFlight = false;  // in case ShutdownServ returned without initiating
        }

    private:
        uint32 _elapsed = 0;
    };
}

void AddSC_custom_graceful_shutdown()
{
    new centurion_graceful_shutdown();
}
