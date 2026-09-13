#include "app/MicrophonePermission.h"

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>

#include <juce_events/juce_events.h>

namespace looper::app
{
MicPermission microphonePermission()
{
    switch ([AVCaptureDevice authorizationStatusForMediaType: AVMediaTypeAudio])
    {
        case AVAuthorizationStatusAuthorized:    return MicPermission::Granted;
        case AVAuthorizationStatusNotDetermined: return MicPermission::NotDetermined;

        // Restricted means policy (parental controls, MDM) rather than the
        // user's own choice. Treated as Denied because the consequence and the
        // remedy are the same — the app cannot prompt its way out of either.
        case AVAuthorizationStatusRestricted:
        case AVAuthorizationStatusDenied:
        default:                                 return MicPermission::Denied;
    }
}

void requestMicrophonePermission(std::function<void(bool granted)> onDone)
{
    if (! onDone)
        return;

    const auto status = microphonePermission();

    // Already settled, either way: answer without involving the OS. Notably a
    // Denied app must not call requestAccessForMediaType — it returns
    // immediately with NO and shows nothing, so "the prompt didn't appear"
    // would look like a bug in this app rather than the OS refusing to ask
    // twice.
    if (status != MicPermission::NotDetermined)
    {
        const bool granted = status == MicPermission::Granted;
        juce::MessageManager::callAsync([onDone, granted] { onDone(granted); });
        return;
    }

    [AVCaptureDevice requestAccessForMediaType: AVMediaTypeAudio
                             completionHandler: ^(BOOL granted)
    {
        // Arrives on an arbitrary queue, so it is bounced to the message
        // thread here — the contract this function documents, and what makes
        // it safe for callers to touch the UI from onDone.
        juce::MessageManager::callAsync([onDone, granted] { onDone(granted == YES); });
    }];
}

bool openMicrophonePrivacySettings()
{
    NSURL* url = [NSURL URLWithString:
        @"x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"];

    if (url == nil)
        return false;

    return [[NSWorkspace sharedWorkspace] openURL: url] == YES;
}

} // namespace looper::app
