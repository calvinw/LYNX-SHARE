#pragma once

// Local control socket (`--control /tmp/lynx.sock`): lets another program on this computer (an MCP server) read and
// edit the open project while LYNX SHARE runs. Wire: newline-delimited JSON-RPC 2.0 over a Unix socket, the same
// framing as Lemon Studio's control plane, so its MCP client can talk to it.
//
// Requests are read on a background thread and run on the message thread (the Song is UI-thread only); every edit is
// one AudioEngine::editSong step, so Ctrl+Z undoes it and the History names it.
//
// Methods: hello, state, notes, apply {commands, name}, play {mode, pattern, from}, pause, stop, transport,
// select {pattern, channel, piano_roll}, undo, redo, history, save {path}.
// apply's ops: add_channel, delete_channel, set_channel, add_pattern, delete_pattern, set_notes, add_notes, add_clip,
// delete_clip, set_tempo. Ids are the entities' stable uids (core/SessionIds.h); positions are ticks (state.ppq).

#include <atomic>
#include <functional>
#include <memory>

#include <juce_core/juce_core.h>

namespace lynx
{
class AudioEngine;

class ControlServer final : private juce::Thread
{
public:
    // What the server needs from the UI (absent ones are skipped).
    struct Hooks
    {
        std::function<void (int patternIndex)> selectPattern;
        std::function<void()> showPianoRoll;
    };

    ControlServer (AudioEngine& engine, juce::String socketPath, Hooks hooks);
    ~ControlServer() override;

    // Binds the socket and starts listening; false (and `error`) when it can't.
    bool start (juce::String& error);

    // One request (a JSON-RPC object as text) handled on the calling thread, which must be the message thread.
    // Returns the response text (no newline). Public for tests.
    juce::String handle (const juce::String& requestLine);

private:
    AudioEngine& engine;
    juce::String path;
    Hooks hooks;
    int listenFd = -1;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    void run() override;
    void serve (int fd);
    juce::var dispatch (const juce::String& method, const juce::var& params);
};
} // namespace lynx
