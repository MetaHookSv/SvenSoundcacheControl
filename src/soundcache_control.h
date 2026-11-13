#pragma once

#include <cctype>
#include <cstddef>
#include <cstring>
#include <string>

namespace svensoundcachecontrol
{
constexpr char        RetryCommand[]     = "retry";
constexpr std::size_t RetryCommandLength = sizeof(RetryCommand) - 1;

// Per-join sound list policy without engine calls, so tests can drive it directly.
// A remote join invalidates the disk list once; when the list is then missing, the
// original loader may reconnect once while its queued dlfile request completes.
class SoundcacheControl
{
public:
    enum class FlushAction
    {
        Forward,  // Call the original flush with its own arguments.
        ForceAll, // Also invalidate the disk sound list.
        Preserve, // Skip the flush to keep a pending recovery download.
    };

    void BeginMap(const char* map, const char* server, bool local)
    {
        // Recovery belongs to one server and map; any other join starts over.
        if (m_Map != map || m_Server != server)
        {
            m_RecoveryPending = false;
            m_FailureWarned   = false;
        }
        m_Map    = map;
        m_Server = server;
        m_Local  = local;
    }

    bool IsLocal() const { return m_Local; }

    // A listen server's loopback client must keep the list its server distributes,
    // and a recovery join must keep the list it has just downloaded.
    bool ShouldInvalidateAtMapStart() const { return !m_Local && !m_RecoveryPending; }

    // Returns false once the single recovery reconnect has been used.
    bool TryStartRecovery()
    {
        if (m_RecoveryPending)
            return false;
        m_RecoveryPending = true;
        return true;
    }

    // Returns true only for the first failed recovery of a join.
    bool TryMarkFailureWarned()
    {
        if (m_FailureWarned)
            return false;
        m_FailureWarned = true;
        return true;
    }

    void FinishLoad(bool loaded)
    {
        if (loaded)
        {
            m_RecoveryPending = false;
            m_FailureWarned   = false;
        }
    }

    FlushAction Flush(bool enabled, bool includingLocal, bool loading) const
    {
        if (!enabled || includingLocal || m_Local)
            return FlushAction::Forward;
        // A missing-list load queues dlfile followed by retry. Preserve that
        // transfer across disconnect, but invalidate the list once consumed.
        if (m_RecoveryPending && !loading)
            return FlushAction::Preserve;
        return FlushAction::ForceAll;
    }

    void Reset() { *this = SoundcacheControl(); }

    // Matches the loader's own reconnect request, which may carry surrounding whitespace.
    static bool IsRetryCommand(const char* command)
    {
        if (!command)
            return false;
        command = SkipSpaces(command);
        if (std::strncmp(command, RetryCommand, RetryCommandLength) != 0)
            return false;
        return *SkipSpaces(command + RetryCommandLength) == '\0';
    }

private:
    static const char* SkipSpaces(const char* text)
    {
        while (std::isspace(static_cast<unsigned char>(*text)))
            ++text;
        return text;
    }

    std::string m_Map;
    std::string m_Server;
    bool        m_Local           = false;
    bool        m_RecoveryPending = false;
    bool        m_FailureWarned   = false;
};
} // namespace svensoundcachecontrol
