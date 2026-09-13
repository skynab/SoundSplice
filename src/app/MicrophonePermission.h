#pragma once

#include <functional>

namespace looper::app
{
/**
    Microphone access, as the OS sees it.

    macOS 10.14+ gates the microphone behind a per-app permission, and JUCE does
    not query or request it (`RuntimePermissions::request` is an Android-only
    implementation that returns true everywhere else — see
    src/app/CMakeLists.txt, where the same gap is already documented for the
    Info.plist key). So this is a small platform shim rather than something the
    framework offers.

    The behaviour that makes it necessary: the OS shows its permission prompt
    *once*, when an app first tries to open an input. This app opens its input
    in the AudioEngine constructor, so that prompt lands at launch — long
    before anyone presses Record, and easy to dismiss without reading. Once
    dismissed, the OS never asks again: CoreAudio simply reports no input
    channels for the rest of the app's life, and nothing can re-trigger the
    prompt from inside the app. The only route back is System Settings, which
    is why openMicrophonePrivacySettings() exists.
*/
enum class MicPermission
{
    /** The OS has never asked. Requesting will show the system prompt. */
    NotDetermined,

    /** Explicitly refused, or blocked by policy. Requesting again does
        nothing at all — the OS will not re-prompt, so the only way forward is
        System Settings. */
    Denied,

    Granted,

    /** No permission model on this platform (everything but macOS today), so
        there is nothing to ask for and nothing that can block recording. */
    NotRequired
};

/** What the OS currently thinks. Cheap; safe to call on the message thread
    whenever a decision depends on it. */
MicPermission microphonePermission();

/**
    Asks the OS to show its permission prompt, and reports the answer.

    Only meaningful when the status is NotDetermined — a Denied app cannot
    re-prompt, by design, and calling this on one reports false immediately
    rather than appearing to ask.

    @p onDone is always called, exactly once, **on the message thread**, so it
    is safe to touch UI from it directly. The system's own completion handler
    arrives on an arbitrary queue; marshalling it here rather than at each call
    site is what keeps that from being every caller's problem to remember.
*/
void requestMicrophonePermission(std::function<void(bool granted)> onDone);

/**
    Opens the OS privacy settings at the microphone list, so a user who has
    already denied access has one click rather than a navigation instruction to
    follow. Returns false if it could not be opened (or there is nothing to
    open on this platform).
*/
bool openMicrophonePrivacySettings();

} // namespace looper::app
