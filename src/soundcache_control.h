#pragma once

#include <cctype>
#include <cstring>
#include <string>

class SoundcacheControl
{
public:
    enum class FlushAction
    {
        Forward,
        ForceAll,
        Preserve
    };

    void BeginMap(const char* map, const char* server, bool local)
    {
        if (m_Map != map || m_Server != server)
        {
            m_Recovery = false;
            m_Warned   = false;
        }
        m_Map    = map;
        m_Server = server;
        m_Local  = local;
    }

    bool InvalidateAtMapStart() const { return !m_Local && !m_Recovery; }

    bool RequestRecovery()
    {
        if (m_Recovery)
            return false;
        m_Recovery = true;
        return true;
    }

    bool WarnRecoveryFailure()
    {
        if (m_Warned)
            return false;
        m_Warned = true;
        return true;
    }

    bool IsLocal() const { return m_Local; }

    void FinishLoad(bool loaded)
    {
        if (loaded)
        {
            m_Recovery = false;
            m_Warned   = false;
        }
    }

    FlushAction Flush(bool enabled, bool includingLocal, bool loading) const
    {
        if (!enabled || includingLocal || m_Local)
            return FlushAction::Forward;
        // A missing-list load queues dlfile followed by retry. Preserve that
        // transfer across disconnect, but invalidate the list once consumed.
        if (m_Recovery && !loading)
            return FlushAction::Preserve;
        return FlushAction::ForceAll;
    }

    void Reset() { *this = SoundcacheControl(); }

    static bool IsRetryCommand(const char* command)
    {
        if (!command)
            return false;
        while (std::isspace(static_cast<unsigned char>(*command)))
            ++command;
        const char retry[] = "retry";
        if (std::strncmp(command, retry, sizeof(retry) - 1))
            return false;
        command += sizeof(retry) - 1;
        while (std::isspace(static_cast<unsigned char>(*command)))
            ++command;
        return !*command;
    }

private:
    std::string m_Map;
    std::string m_Server;
    bool        m_Local    = false;
    bool        m_Recovery = false;
    bool        m_Warned   = false;
};
