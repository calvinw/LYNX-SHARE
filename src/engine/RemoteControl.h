#pragma once

#include <functional>
#include <memory>
#include <juce_core/juce_core.h>

namespace lynx
{
class AudioEngine;

// Opt-in, vendor-neutral local control. Disabled means no listener or discovery
// file. The engine and callbacks must outlive this object.
class RemoteControl final
{
public:
    static std::unique_ptr<RemoteControl> create (AudioEngine&, bool enabled, std::function<juce::String()> projectName,
                                                 std::function<juce::String()> projectPath = {});
    ~RemoteControl();
    bool active() const noexcept;
    juce::File discoveryFile() const;

    // What `ui.select` and `project.save` need from the window (message thread only). Absent hooks make those
    // operations report that they are unavailable. Set once by the app; shared by every RemoteControl instance.
    struct UiHooks
    {
        std::function<void (int patternIndex)> selectPattern;
        std::function<void()> showPianoRoll;
        // Saves to `file` (or the current document when `file` is empty) like File › Save; returns the saved file,
        // or throws std::runtime_error with the reason.
        std::function<juce::File (const juce::File& file)> save;
        // Starts LYNX's default new project (File › New without its dialog), saved to `file` when given.
        std::function<void (const juce::File& file)> newProject;
    };
    static void setUiHooks (UiHooks hooks);

private:
    struct Impl;
    explicit RemoteControl (std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl;
};
} // namespace lynx
