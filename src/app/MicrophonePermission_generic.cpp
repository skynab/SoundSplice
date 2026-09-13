#include "app/MicrophonePermission.h"

#include <juce_events/juce_events.h>

namespace looper::app
{
// Windows and Linux have no per-app microphone gate an application can query
// or trigger from inside itself: Windows' privacy settings block the device at
// the OS level (the app simply sees no input device, which the existing
// "no audio input" path already reports), and Linux has no such model at all.
//
// So this reports NotRequired rather than pretending to be Granted. The
// difference matters at the call site: "the OS says yes" and "there is nothing
// to ask" lead to the same recording behaviour but to different messages, and
// telling a Windows user to check a macOS privacy pane would be worse than
// saying nothing.

MicPermission microphonePermission()
{
    return MicPermission::NotRequired;
}

void requestMicrophonePermission(std::function<void(bool granted)> onDone)
{
    if (! onDone)
        return;

    // Still asynchronous, and still on the message thread: a platform
    // difference must not change *when* a caller's continuation runs, or this
    // would be the one platform where the retry happens re-entrantly.
    juce::MessageManager::callAsync([onDone] { onDone(true); });
}

bool openMicrophonePrivacySettings()
{
    return false;
}

} // namespace looper::app
