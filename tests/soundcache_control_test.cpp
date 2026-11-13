#include "soundcache_control.h"
#include <cstdio>

namespace
{
using svensoundcachecontrol::SoundcacheControl;
using Action = SoundcacheControl::FlushAction;

int failures = 0;

void Check(const char* label, int expected, int actual)
{
    if (expected != actual)
    {
        std::printf("FAIL %s: expected %d, got %d\n", label, expected, actual);
        ++failures;
    }
}

void Check(const char* label, Action expected, Action actual)
{
    Check(label, static_cast<int>(expected), static_cast<int>(actual));
}

void Startup()
{
    SoundcacheControl control;
    Check("startup discards stale disk lists", Action::ForceAll, control.Flush(true, false, false));
    Check("disabled mode forwards original arguments", Action::Forward, control.Flush(false, false, false));
    Check("explicit full flush stays explicit", Action::Forward, control.Flush(true, true, false));
}

void Recovery()
{
    SoundcacheControl control;
    control.BeginMap("maps/test.bsp", "server-a", false);
    Check("new join invalidates even an asynchronously recreated file", 1, control.ShouldInvalidateAtMapStart());
    Check("successful remote load removes disk list for next join", Action::ForceAll, control.Flush(true, false, true));
    Check("missing list allows one SDK recovery retry", 1, control.TryStartRecovery());
    Check("download failure cannot cause an automatic reconnect loop", 0, control.TryStartRecovery());
    Check("first exhausted retry is reported", 1, control.TryMarkFailureWarned());
    Check("download failure diagnostic is not repeated", 0, control.TryMarkFailureWarned());
    Check("SDK retry preserves pending download", Action::Preserve, control.Flush(true, false, false));
    Check("explicit all overrides recovery protection", Action::Forward, control.Flush(true, true, false));
    Check("disabling overrides recovery protection", Action::Forward, control.Flush(false, false, false));

    control.BeginMap("maps/test.bsp", "server-a", false);
    Check("recovery join does not delete the new download", 0, control.ShouldInvalidateAtMapStart());
    Check("same server same map recovery survives reconnect", Action::Preserve, control.Flush(true, false, false));
    control.FinishLoad(false);
    Check("failed load keeps download protection", Action::Preserve, control.Flush(true, false, false));
    Check("loader final flush still invalidates successfully consumed list", Action::ForceAll, control.Flush(true, false, true));
    control.FinishLoad(true);
    Check("next user join refreshes the same map again", 1, control.ShouldInvalidateAtMapStart());
    Check("successful load ends recovery", Action::ForceAll, control.Flush(true, false, false));

    control.TryStartRecovery();
    control.BeginMap("maps/test.bsp", "server-b", false);
    Check("server change cannot inherit recovery", Action::ForceAll, control.Flush(true, false, false));
    control.TryStartRecovery();
    control.BeginMap("maps/other.bsp", "server-b", false);
    Check("map change cannot inherit recovery", Action::ForceAll, control.Flush(true, false, false));

    control.Reset();
    Check("shutdown reset removes stale local and recovery state", Action::ForceAll, control.Flush(true, false, false));
}

void ListenServer()
{
    SoundcacheControl control;
    control.BeginMap("maps/test.bsp", "loopback", true);
    Check("listen server never deletes its active server list", 0, control.ShouldInvalidateAtMapStart());
    Check("listen server keeps generated file available to other clients", Action::Forward, control.Flush(true, false, true));
    control.TryStartRecovery();
    Check("listen server keeps vanilla cleanup", Action::Forward, control.Flush(true, false, false));
}

void RetryCommand()
{
    Check("SDK retry without newline", 1, SoundcacheControl::IsRetryCommand("retry"));
    Check("SDK retry with whitespace", 1, SoundcacheControl::IsRetryCommand(" \tretry\r\n"));
    Check("command prefix is not retry", 0, SoundcacheControl::IsRetryCommand("retry_extra"));
    Check("unrelated command is not retry", 0, SoundcacheControl::IsRetryCommand("echo retry"));
    Check("null command is not retry", 0, SoundcacheControl::IsRetryCommand(nullptr));
}
} // namespace

int main()
{
    Startup();
    Recovery();
    ListenServer();
    RetryCommand();
    std::printf("SvenSoundcacheControl behavior: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
