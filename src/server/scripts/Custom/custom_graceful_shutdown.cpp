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
 */

#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "World.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace
{
    std::string s_requestFile;
    uint32 s_pollMs = 3000;

    void LoadShutdownConfig()
    {
        s_requestFile = sConfigMgr->GetStringDefault("Centurion.Shutdown.RequestFile", "");
        s_pollMs = uint32(std::max(500, sConfigMgr->GetIntDefault("Centurion.Shutdown.PollMs", 3000)));

        if (!s_requestFile.empty())
            TC_LOG_INFO("server.worldserver",
                "Graceful shutdown requests are read from '{}' every {}ms.", s_requestFile, s_pollMs);
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

            sWorld->ShutdownServ(seconds, 0, SHUTDOWN_EXIT_CODE, message);
        }

    private:
        uint32 _elapsed = 0;
    };
}

void AddSC_custom_graceful_shutdown()
{
    new centurion_graceful_shutdown();
}
