#include "soundcache_control.h"
#include <cstdio>
#include <cstdlib>

static void Check(bool expected, bool actual, const char* scenario)
{
    if (expected != actual)
    {
        std::fprintf(stderr, "FAILED: %s\n", scenario);
        std::exit(EXIT_FAILURE);
    }
}

int main()
{
    using Action = SoundcacheControl::FlushAction;
    SoundcacheControl control;

    Check(true, Action::ForceAll == control.Flush(true, false, false), "startup discards stale disk lists");
    Check(true, Action::Forward == control.Flush(false, false, false), "disabled mode forwards original arguments");
    Check(true, Action::Forward == control.Flush(true, true, false), "explicit full flush stays explicit");

    control.BeginMap("maps/test.bsp", "server-a", false);
    Check(true, control.InvalidateAtMapStart(), "new join invalidates even an asynchronously recreated file");
    Check(true, Action::ForceAll == control.Flush(true, false, true), "successful remote load removes disk list for next join");
    Check(true, control.RequestRecovery(), "missing list allows one SDK recovery retry");
    Check(false, control.RequestRecovery(), "download failure cannot cause an automatic reconnect loop");
    Check(true, control.WarnRecoveryFailure(), "first exhausted retry is reported");
    Check(false, control.WarnRecoveryFailure(), "download failure diagnostic is not repeated");
    Check(true, Action::Preserve == control.Flush(true, false, false), "SDK retry preserves pending download");
    Check(true, Action::Forward == control.Flush(true, true, false), "explicit all overrides recovery protection");
    Check(true, Action::Forward == control.Flush(false, false, false), "disabling overrides recovery protection");

    control.BeginMap("maps/test.bsp", "server-a", false);
    Check(false, control.InvalidateAtMapStart(), "recovery join does not delete the new download");
    Check(true, Action::Preserve == control.Flush(true, false, false), "same server same map recovery survives reconnect");
    control.FinishLoad(false);
    Check(true, Action::Preserve == control.Flush(true, false, false), "failed load keeps download protection");
    Check(true, Action::ForceAll == control.Flush(true, false, true), "loader final flush still invalidates successfully consumed list");
    control.FinishLoad(true);
    Check(true, control.InvalidateAtMapStart(), "next user join refreshes the same map again");
    Check(true, Action::ForceAll == control.Flush(true, false, false), "successful load ends recovery");

    control.RequestRecovery();
    control.BeginMap("maps/test.bsp", "server-b", false);
    Check(true, Action::ForceAll == control.Flush(true, false, false), "server change cannot inherit recovery");
    control.RequestRecovery();
    control.BeginMap("maps/other.bsp", "server-b", false);
    Check(true, Action::ForceAll == control.Flush(true, false, false), "map change cannot inherit recovery");

    control.BeginMap("maps/test.bsp", "loopback", true);
    Check(false, control.InvalidateAtMapStart(), "listen server never deletes its active server list");
    Check(true, Action::Forward == control.Flush(true, false, true), "listen server keeps generated file available to other clients");
    control.RequestRecovery();
    Check(true, Action::Forward == control.Flush(true, false, false), "listen server keeps vanilla cleanup");

    Check(true, SoundcacheControl::IsRetryCommand("retry"), "SDK retry without newline");
    Check(true, SoundcacheControl::IsRetryCommand(" \tretry\r\n"), "SDK retry with whitespace");
    Check(false, SoundcacheControl::IsRetryCommand("retry_extra"), "command prefix is not retry");
    Check(false, SoundcacheControl::IsRetryCommand("echo retry"), "unrelated command is not retry");
    Check(false, SoundcacheControl::IsRetryCommand(nullptr), "null command is not retry");

    control.Reset();
    Check(true, Action::ForceAll == control.Flush(true, false, false), "shutdown reset removes stale local and recovery state");
    std::puts("Soundcache behavior scenarios passed.");
    return EXIT_SUCCESS;
}
