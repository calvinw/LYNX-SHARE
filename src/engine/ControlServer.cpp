#include "engine/ControlServer.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>

#include <juce_events/juce_events.h>

#include "core/History.h"
#include "core/RackOps.h"
#include "core/SessionIds.h"
#include "core/Song.h"
#include "engine/AudioEngine.h"
#include "engine/PluginHost.h"
#include "engine/ProjectFile.h"

#if ! JUCE_WINDOWS
 #include <cerrno>
 #include <cstring>
 #include <poll.h>
 #include <sys/socket.h>
 #include <sys/stat.h>
 #include <sys/un.h>
 #include <unistd.h>
#endif

namespace lynx
{
namespace
{
struct RpcError
{
    int code;
    juce::String message;
};

[[noreturn]] void fail (const juce::String& message, int code = -32000) { throw RpcError { code, message }; }

// --- JSON helpers -----------------------------------------------------------------------------------------------------

struct Obj
{
    juce::DynamicObject::Ptr o = new juce::DynamicObject();
    Obj& set (const char* key, const juce::var& v)
    {
        o->setProperty (key, v);
        return *this;
    }
    operator juce::var() const { return juce::var (o.get()); }
};

juce::var uidVar (std::uint64_t uid) { return juce::var ((juce::int64) uid); }

bool has (const juce::var& params, const char* key)
{
    return params.hasProperty (key) && ! params[key].isVoid() && ! params[key].isUndefined();
}

std::int64_t intArg (const juce::var& params, const char* key)
{
    if (! has (params, key))
        fail (juce::String ("missing `") + key + "`", -32602);
    return (std::int64_t) (juce::int64) params[key];
}

std::uint64_t idArg (const juce::var& params, const char* key) { return (std::uint64_t) intArg (params, key); }

int patternIndex (const Song& s, std::uint64_t uid)
{
    for (std::size_t i = 0; i < s.patterns.size(); ++i)
        if (s.patterns[i].uid == uid)
            return (int) i;
    fail ("no pattern with id " + juce::String ((juce::int64) uid));
}

int channelIndex (const Song& s, std::uint64_t uid)
{
    for (std::size_t i = 0; i < s.channels.size(); ++i)
        if (s.channels[i].uid == uid)
            return (int) i;
    fail ("no channel with id " + juce::String ((juce::int64) uid));
}

int clipIndex (const Song& s, std::uint64_t uid)
{
    for (std::size_t i = 0; i < s.playlist.size(); ++i)
        if (s.playlist[i].uid == uid)
            return (int) i;
    fail ("no clip with id " + juce::String ((juce::int64) uid));
}

std::uint64_t channelUidAt (const Song& s, int index)
{
    return index >= 0 && index < (int) s.channels.size() ? s.channels[(std::size_t) index].uid : 0;
}

// The built-in instruments a new channel can be made with.
const std::pair<const char*, InstrumentKind> instrumentNames[] = {
    { "synth", InstrumentKind::synth }, { "kick", InstrumentKind::kick },   { "snare", InstrumentKind::snare },
    { "hat", InstrumentKind::hat },     { "clap", InstrumentKind::clap },   { "sampler", InstrumentKind::sampler },
};

juce::String instrumentName (InstrumentKind k)
{
    for (const auto& [name, kind] : instrumentNames)
        if (kind == k)
            return name;
    return "other:" + juce::String ((int) k);
}

// Velocity and pan use MIDI-style 0..128 on the wire (64 = centre), like Lemon Studio; LYNX stores 0..1 and -1..1.
juce::var noteVar (const Song& s, const Note& n)
{
    return Obj()
        .set ("channel", uidVar (channelUidAt (s, n.channel)))
        .set ("position", (juce::int64) n.start)
        .set ("length", (juce::int64) n.length)
        .set ("key", n.key)
        .set ("velocity", juce::roundToInt (n.velocity * 128.0f))
        .set ("pan", juce::roundToInt (n.pan * 64.0f + 64.0f))
        .set ("muted", n.muted);
}

Note noteFrom (const Song& s, const juce::var& v, const juce::var& defaultChannel)
{
    Note n;
    const auto ch = has (v, "channel") ? v["channel"] : defaultChannel;
    if (ch.isVoid() || ch.isUndefined())
        fail ("each note needs a `channel`", -32602);
    n.channel = channelIndex (s, (std::uint64_t) (juce::int64) ch);
    n.start = std::max<std::int64_t> (0, has (v, "position") ? (juce::int64) v["position"] : 0);
    n.length = std::max<std::int64_t> (1, has (v, "length") ? (juce::int64) v["length"] : ticksPerStep);
    n.key = juce::jlimit (0, 131, has (v, "key") ? (int) v["key"] : 60);
    n.velocity = juce::jlimit (0.0f, 1.0f, (has (v, "velocity") ? (float) v["velocity"] : 100.0f) / 128.0f);
    n.pan = juce::jlimit (-1.0f, 1.0f, ((has (v, "pan") ? (float) v["pan"] : 64.0f) - 64.0f) / 64.0f);
    n.muted = has (v, "muted") && (bool) v["muted"];
    return n;
}

// A scanned plugin by name (case-insensitive; a VST3 is preferred over the other formats), or by identifier string.
std::optional<juce::PluginDescription> findPlugin (AudioEngine& engine, const juce::String& name, bool instrument)
{
    std::optional<juce::PluginDescription> best;
    for (const auto& t : engine.getPluginHost().getKnownPlugins().getTypes())
    {
        if (t.isInstrument != instrument)
            continue;
        if (t.createIdentifierString() == name)
            return t;
        if (t.name.equalsIgnoreCase (name) && (! best || t.pluginFormatName == "VST3"))
            best = t;
    }
    return best;
}

// --- apply: one op on the working copy ----------------------------------------------------------------------------------

// Returns the op's result; uids of new things are filled in after the copy is normalized (`fixups`).
juce::var applyOp (AudioEngine& engine, Song& s, const juce::var& op, std::vector<std::function<void()>>& fixups)
{
    const auto name = op["op"].toString();

    if (name == "add_channel")
    {
        ChannelDef c;
        c.instrument = InstrumentKind::synth;
        const auto kind = has (op, "instrument") ? op["instrument"].toString() : has (op, "plugin") ? op["plugin"].toString() : juce::String();
        if (kind.isNotEmpty())
        {
            bool found = false;
            for (const auto& [n, k] : instrumentNames)
                if (kind.equalsIgnoreCase (n))
                    c.instrument = k, found = true;
            if (! found)
            {
                const auto plugin = findPlugin (engine, kind, true);
                if (! plugin)
                    fail ("unknown instrument `" + kind + "`: not built in (synth, kick, snare, hat, clap, sampler) and no "
                          "scanned instrument plugin has that name (see `plugins`)", -32602);
                c.instrument = InstrumentKind::plugin;
                c.plugin.id = plugin->createIdentifierString().toStdString();
                c.plugin.name = plugin->name.toStdString();
            }
        }
        if (has (op, "sample"))
        {
            const juce::File f (op["sample"].toString());
            if (! f.existsAsFile())
                fail ("no such sample file: " + f.getFullPathName(), -32602);
            c.instrument = InstrumentKind::sampler;
            c.samplePath = f.getFullPathName().toStdString();
        }
        c.name = has (op, "name") ? op["name"].toString().toStdString()
                 : ! c.samplePath.empty()     ? juce::File (c.samplePath).getFileNameWithoutExtension().toStdString()
                 : ! c.plugin.name.empty()    ? c.plugin.name
                                              : instrumentName (c.instrument).toStdString();
        const int track = has (op, "track") ? (int) op["track"] : -1;
        const int index = addChannelRouted (s, c, track);
        auto result = std::make_shared<Obj>();
        result->set ("index", index);
        fixups.push_back ([&s, index, result] { result->set ("id", uidVar (s.channels[(std::size_t) index].uid)); });
        return juce::var (result->o.get());
    }
    if (name == "delete_channel")
    {
        s.removeChannel (channelIndex (s, idArg (op, "channel")));
        return {};
    }
    if (name == "set_channel")
    {
        auto& c = s.channels[(std::size_t) channelIndex (s, idArg (op, "channel"))];
        if (has (op, "name"))
            c.name = op["name"].toString().toStdString();
        if (has (op, "volume"))
            c.volume = juce::jlimit (0.0f, 1.0f, (float) op["volume"]);
        if (has (op, "pan"))
            c.pan = juce::jlimit (-1.0f, 1.0f, (float) op["pan"]);
        if (has (op, "muted"))
            c.muted = (bool) op["muted"];
        if (has (op, "track"))
            c.mixerTrack = juce::jlimit (0, std::max (0, (int) s.mixer.size() - 1), (int) op["track"]);
        return {};
    }
    if (name == "set_synth")
    {
        // the built-in 3-oscillator synth's sound (a channel whose instrument is "synth"); fields left out stay
        auto& c = s.channels[(std::size_t) channelIndex (s, idArg (op, "channel"))];
        if (c.instrument != InstrumentKind::synth)
            fail ("set_synth needs a channel with the built-in synth", -32602);
        auto& y = c.synth;
        const auto num = [&op] (const juce::var& v, const char* key, float& out, float lo, float hi) {
            juce::ignoreUnused (op);
            if (has (v, key))
                out = juce::jlimit (lo, hi, (float) v[key]);
        };
        if (const auto* oscs = op["osc"].getArray())
        {
            if (oscs->size() > 3)
                fail ("at most 3 oscillators", -32602);
            for (int i = 0; i < oscs->size(); ++i)
            {
                const auto& v = oscs->getReference (i);
                auto& o = y.osc[(std::size_t) i];
                if (has (v, "wave"))
                {
                    static const std::pair<const char*, Waveform> waves[] = { { "sine", Waveform::sine }, { "triangle", Waveform::triangle },
                                                                              { "saw", Waveform::saw }, { "square", Waveform::square },
                                                                              { "noise", Waveform::noise } };
                    bool found = false;
                    for (const auto& [n, w] : waves)
                        if (v["wave"].toString().equalsIgnoreCase (n))
                            o.wave = w, found = true;
                    if (! found)
                        fail ("wave is sine, triangle, saw, square or noise", -32602);
                }
                if (has (v, "octave"))
                    o.octave = juce::jlimit (-3, 3, (int) v["octave"]);
                if (has (v, "semitones"))
                    o.semitones = juce::jlimit (-11, 11, (int) v["semitones"]);
                num (v, "detune", o.detune, -100.0f, 100.0f);
                num (v, "level", o.level, 0.0f, 1.0f);
                num (v, "pan", o.pan, -1.0f, 1.0f);
            }
        }
        num (op, "attack", y.attack, 0.0f, 10.0f);
        num (op, "decay", y.decay, 0.0f, 10.0f);
        num (op, "sustain", y.sustain, 0.0f, 1.0f);
        num (op, "release", y.release, 0.0f, 10.0f);
        num (op, "cutoff", y.cutoff, 20.0f, 20000.0f);
        num (op, "resonance", y.resonance, 0.0f, 1.0f);
        num (op, "key_track", y.keyTrack, 0.0f, 2.0f);
        if (has (op, "filter_type"))
        {
            const auto t = op["filter_type"].toString();
            y.filterType = t == "bandpass" ? 1 : t == "highpass" ? 2 : t == "bandstop" ? 3 : 0;
        }
        if (has (op, "ladder"))
            y.ladder = (bool) op["ladder"];
        if (has (op, "mono"))
            y.mono = (bool) op["mono"];
        if (has (op, "porta"))
            y.porta = (bool) op["porta"];
        num (op, "porta_beats", y.portaBeats, 0.0f, 4.0f);
        if (has (op, "chorus"))
            y.chorus = juce::jlimit (0, 3, (int) op["chorus"]);
        if (has (op, "filter_env"))
        {
            const auto& v = op["filter_env"];
            num (v, "amount", y.filterEnv.amount, -10.0f, 10.0f); // octaves
            num (v, "attack", y.filterEnv.attack, 0.0f, 10.0f);
            num (v, "hold", y.filterEnv.hold, 0.0f, 10.0f);
            num (v, "decay", y.filterEnv.decay, 0.0f, 10.0f);
            num (v, "sustain", y.filterEnv.sustain, 0.0f, 1.0f);
            num (v, "release", y.filterEnv.release, 0.0f, 10.0f);
        }
        return {};
    }
    if (name == "add_pattern")
    {
        Pattern p;
        p.name = has (op, "name") ? op["name"].toString().toStdString() : newPatternName (s);
        s.patterns.push_back (std::move (p));
        const int index = (int) s.patterns.size() - 1;
        auto result = std::make_shared<Obj>();
        result->set ("index", index);
        fixups.push_back ([&s, index, result] { result->set ("id", uidVar (s.patterns[(std::size_t) index].uid)); });
        return juce::var (result->o.get());
    }
    if (name == "delete_pattern")
    {
        s.removePattern (patternIndex (s, idArg (op, "pattern")));
        return {};
    }
    if (name == "set_pattern")
    {
        auto& p = s.patterns[(std::size_t) patternIndex (s, idArg (op, "pattern"))];
        if (has (op, "name"))
            p.name = op["name"].toString().toStdString();
        return {};
    }
    if (name == "set_notes" || name == "add_notes")
    {
        auto& p = s.patterns[(std::size_t) patternIndex (s, idArg (op, "pattern"))];
        const auto defaultChannel = has (op, "channel") ? op["channel"] : juce::var();
        std::vector<Note> incoming;
        if (const auto* list = op["notes"].getArray())
            for (const auto& v : *list)
                incoming.push_back (noteFrom (s, v, defaultChannel));
        if (name == "set_notes")
        {
            if (has (op, "channel"))
            {
                const int ch = channelIndex (s, idArg (op, "channel"));
                std::erase_if (p.notes, [ch] (const Note& n) { return n.channel == ch; });
            }
            else
            {
                p.notes.clear();
            }
        }
        p.notes.insert (p.notes.end(), incoming.begin(), incoming.end());
        p.sortNotes();
        return Obj().set ("written", (int) incoming.size());
    }
    if (name == "add_clip")
    {
        const int pi = patternIndex (s, idArg (op, "pattern"));
        Clip c;
        c.kind = ClipKind::pattern;
        c.pattern = pi;
        c.track = std::max (0, has (op, "track") ? (int) op["track"] : 0);
        c.start = std::max<std::int64_t> (0, has (op, "position") ? (juce::int64) op["position"] : 0);
        c.length = has (op, "length") ? std::max<std::int64_t> (1, (juce::int64) op["length"])
                                      : std::max<std::int64_t> (1, s.patterns[(std::size_t) pi].effectiveLength (s.beatsPerBar));
        s.playlist.push_back (c);
        const int index = (int) s.playlist.size() - 1;
        auto result = std::make_shared<Obj>();
        fixups.push_back ([&s, index, result] { result->set ("id", uidVar (s.playlist[(std::size_t) index].uid)); });
        return juce::var (result->o.get());
    }
    if (name == "delete_clip")
    {
        s.playlist.erase (s.playlist.begin() + clipIndex (s, idArg (op, "clip")));
        return {};
    }
    if (name == "set_tempo")
    {
        if (! has (op, "bpm"))
            fail ("missing `bpm`", -32602);
        s.bpm = juce::jlimit (10.0, 999.0, (double) op["bpm"]);
        return {};
    }
    fail ("unknown op `" + name + "`", -32601);
}

juce::var stateVar (const AudioEngine& engine)
{
    const auto& s = engine.getSong();
    juce::Array<juce::var> channels, patterns, clips, mixer;
    for (std::size_t i = 0; i < s.channels.size(); ++i)
    {
        const auto& c = s.channels[i];
        channels.add (Obj()
                          .set ("id", uidVar (c.uid))
                          .set ("index", (int) i)
                          .set ("name", juce::String::fromUTF8 (c.name.c_str()))
                          .set ("instrument", c.instrument == InstrumentKind::plugin ? juce::String::fromUTF8 (c.plugin.name.c_str())
                                                                                     : instrumentName (c.instrument))
                          .set ("track", c.mixerTrack)
                          .set ("volume", c.volume)
                          .set ("pan", c.pan)
                          .set ("muted", c.muted));
    }
    for (std::size_t i = 0; i < s.patterns.size(); ++i)
    {
        const auto& p = s.patterns[i];
        patterns.add (Obj()
                          .set ("id", uidVar (p.uid))
                          .set ("index", (int) i)
                          .set ("name", juce::String::fromUTF8 (p.name.c_str()))
                          .set ("length", (juce::int64) p.effectiveLength (s.beatsPerBar))
                          .set ("notes", (int) p.notes.size()));
    }
    for (const auto& c : s.playlist)
    {
        Obj o;
        o.set ("id", uidVar (c.uid)).set ("track", c.track).set ("position", (juce::int64) c.start).set ("length", (juce::int64) c.length);
        if (c.kind == ClipKind::pattern && c.pattern >= 0 && c.pattern < (int) s.patterns.size())
            o.set ("pattern", uidVar (s.patterns[(std::size_t) c.pattern].uid));
        else
            o.set ("kind", (int) c.kind);
        clips.add (o);
    }
    for (std::size_t i = 0; i < s.mixer.size(); ++i)
        mixer.add (Obj().set ("index", (int) i).set ("name", juce::String::fromUTF8 (s.mixer[i].name.c_str())));

    const auto current = s.currentPattern >= 0 && s.currentPattern < (int) s.patterns.size() ? s.patterns[(std::size_t) s.currentPattern].uid : 0;
    return Obj()
        .set ("ppq", ppq)
        .set ("bpm", s.bpm)
        .set ("beats_per_bar", s.beatsPerBar)
        .set ("mode", s.mode == PlayMode::song ? "song" : "pattern")
        .set ("current_pattern", uidVar (current))
        .set ("selected_channel", uidVar (channelUidAt (s, engine.getSelectedChannel())))
        .set ("channels", channels)
        .set ("patterns", patterns)
        .set ("clips", clips)
        .set ("mixer", mixer);
}

juce::var transportVar (AudioEngine& engine)
{
    const auto& t = engine.getTransport();
    return Obj()
        .set ("playing", t.playing.load())
        .set ("position", t.positionTicks.load())
        .set ("mode", engine.getSong().mode == PlayMode::song ? "song" : "pattern")
        .set ("bpm", engine.getSong().bpm)
        .set ("ppq", ppq);
}
} // namespace

// --- requests -----------------------------------------------------------------------------------------------------------

ControlServer::ControlServer (AudioEngine& e, juce::String socketPath, Hooks h)
    : juce::Thread ("LYNX control"), engine (e), path (std::move (socketPath)), hooks (std::move (h))
{
}

juce::var ControlServer::dispatch (const juce::String& method, const juce::var& params)
{
    if (method == "hello")
        return Obj().set ("app", "LYNX SHARE").set ("protocol", 1).set ("ppq", ppq);
    if (method == "state")
        return stateVar (engine);
    if (method == "transport")
        return transportVar (engine);
    if (method == "plugins")
    {
        juce::Array<juce::var> list;
        for (const auto& t : engine.getPluginHost().getKnownPlugins().getTypes())
            list.add (Obj()
                          .set ("name", t.name)
                          .set ("manufacturer", t.manufacturerName)
                          .set ("format", t.pluginFormatName)
                          .set ("instrument", t.isInstrument)
                          .set ("id", t.createIdentifierString()));
        return Obj().set ("plugins", list);
    }
    if (method == "notes")
    {
        const auto& s = engine.getSong();
        const auto& p = s.patterns[(std::size_t) patternIndex (s, idArg (params, "pattern"))];
        juce::Array<juce::var> notes;
        for (const auto& n : p.notes)
            notes.add (noteVar (s, n));
        return Obj().set ("pattern", uidVar (p.uid)).set ("notes", notes);
    }
    if (method == "apply")
    {
        const auto* commands = params["commands"].getArray();
        if (commands == nullptr || commands->isEmpty())
            fail ("`commands` must be a non-empty list", -32602);
        // Deleting channels moves plugins: their live settings go into the Song first (as editSongStructural does).
        for (const auto& c : *commands)
            if (c["op"].toString() == "delete_channel")
            {
                engine.capturePluginStates();
                break;
            }
        // All or nothing: the ops run on a copy, which replaces the song in one undo step only if every op worked.
        Song copy = engine.getSong();
        std::vector<std::function<void()>> fixups;
        juce::Array<juce::var> results;
        for (const auto& c : *commands)
            results.add (applyOp (engine, copy, c, fixups));
        uids::normalize (copy);
        for (auto& f : fixups)
            f();
        engine.editSong ([&copy] (Song& s) { s = std::move (copy); });
        return Obj().set ("results", results);
    }
    if (method == "play")
    {
        if (has (params, "mode") || has (params, "pattern"))
        {
            const auto mode = params["mode"].toString();
            if (mode.isNotEmpty() && mode != "song" && mode != "pattern")
                fail ("mode is \"song\" or \"pattern\"", -32602);
            const int pi = has (params, "pattern") ? patternIndex (engine.getSong(), idArg (params, "pattern")) : -1;
            engine.editSong ([&] (Song& s) {
                if (mode.isNotEmpty())
                    s.mode = mode == "song" ? PlayMode::song : PlayMode::pattern;
                if (pi >= 0)
                    s.currentPattern = pi;
            }, false);
        }
        if (has (params, "from"))
            engine.seek ((double) params["from"]);
        engine.play();
        return transportVar (engine);
    }
    if (method == "stop")
    {
        engine.stop();
        return transportVar (engine);
    }
    if (method == "pause")
    {
        const auto at = engine.getTransport().positionTicks.load();
        engine.stop();
        engine.seek (at);
        return transportVar (engine);
    }
    if (method == "select")
    {
        const auto& s = engine.getSong();
        if (has (params, "pattern"))
        {
            const int pi = patternIndex (s, idArg (params, "pattern"));
            if (hooks.selectPattern)
                hooks.selectPattern (pi);
            else
                engine.editSong ([pi] (Song& song) { song.currentPattern = pi; }, false);
        }
        if (has (params, "channel"))
            engine.setSelectedChannel (channelIndex (engine.getSong(), idArg (params, "channel")));
        if (has (params, "piano_roll") && (bool) params["piano_roll"] && hooks.showPianoRoll)
            hooks.showPianoRoll();
        return stateVar (engine);
    }
    if (method == "undo" || method == "redo")
    {
        if (method == "undo" ? ! engine.canUndo() : ! engine.canRedo())
            fail ("nothing to " + method);
        method == "undo" ? engine.undo() : engine.redo();
        return Obj().set ("position", engine.getHistoryPosition());
    }
    if (method == "history")
    {
        juce::Array<juce::var> steps;
        for (int i = 0; i < engine.getHistorySize(); ++i)
        {
            const auto label = engine.getHistoryLabel (i);
            steps.add (juce::String (label.text).replace ("{0}", juce::String::fromUTF8 (label.arg.c_str())));
        }
        return Obj().set ("steps", steps).set ("position", engine.getHistoryPosition());
    }
    if (method == "save")
    {
        if (! has (params, "path"))
            fail ("give a `path` to save to (.lynx)", -32602);
        const juce::File file (params["path"].toString());
        juce::String error;
        engine.capturePluginStates();
        if (! ProjectFile::save (engine.getSong(), file, error))
            fail ("could not save: " + error);
        return Obj().set ("path", file.getFullPathName());
    }
    fail ("unknown method `" + method + "`", -32601);
}

juce::String ControlServer::handle (const juce::String& line)
{
    juce::var id;
    Obj response;
    response.set ("jsonrpc", "2.0");
    try
    {
        juce::var request;
        if (juce::JSON::parse (line, request).failed() || ! request.isObject())
            fail ("parse error", -32700);
        id = request["id"];
        response.set ("result", dispatch (request["method"].toString(), request["params"]));
    }
    catch (const RpcError& e)
    {
        response.set ("error", Obj().set ("code", e.code).set ("message", e.message));
    }
    catch (const std::exception& e)
    {
        response.set ("error", Obj().set ("code", -32603).set ("message", juce::String (e.what())));
    }
    response.set ("id", id);
    return juce::JSON::toString (response, true);
}

// --- socket -------------------------------------------------------------------------------------------------------------

#if JUCE_WINDOWS

bool ControlServer::start (juce::String& error)
{
    error = "the control socket is not available on Windows yet";
    return false;
}
ControlServer::~ControlServer() { *alive = false; }
void ControlServer::run() {}
void ControlServer::serve (int) {}

#else

bool ControlServer::start (juce::String& error)
{
    sockaddr_un addr {};
    if ((size_t) path.getNumBytesAsUTF8() >= sizeof (addr.sun_path))
    {
        error = "socket path too long";
        return false;
    }
    listenFd = ::socket (AF_UNIX, SOCK_STREAM, 0);
    if (listenFd < 0)
    {
        error = std::strerror (errno);
        return false;
    }
    addr.sun_family = AF_UNIX;
    std::strncpy (addr.sun_path, path.toRawUTF8(), sizeof (addr.sun_path) - 1);
    ::unlink (addr.sun_path); // a socket left by an earlier run
    if (::bind (listenFd, (sockaddr*) &addr, sizeof (addr)) != 0 || ::listen (listenFd, 4) != 0)
    {
        error = std::strerror (errno);
        ::close (listenFd);
        listenFd = -1;
        return false;
    }
    ::chmod (addr.sun_path, 0600); // only this user
    startThread();
    return true;
}

ControlServer::~ControlServer()
{
    *alive = false;
    signalThreadShouldExit();
    stopThread (2000);
    if (listenFd >= 0)
    {
        ::close (listenFd);
        ::unlink (path.toRawUTF8());
    }
}

void ControlServer::run()
{
    while (! threadShouldExit())
    {
        pollfd p { listenFd, POLLIN, 0 };
        if (::poll (&p, 1, 200) <= 0)
            continue;
        const int fd = ::accept (listenFd, nullptr, nullptr);
        if (fd < 0)
            continue;
   #ifdef SO_NOSIGPIPE
        int one = 1;
        ::setsockopt (fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
   #endif
        serve (fd); // one client at a time
        ::close (fd);
    }
}

void ControlServer::serve (int fd)
{
   #ifdef MSG_NOSIGNAL
    constexpr int sendFlags = MSG_NOSIGNAL;
   #else
    constexpr int sendFlags = 0;
   #endif
    std::string buffer;
    char chunk[4096];

    while (! threadShouldExit())
    {
        pollfd p { fd, POLLIN, 0 };
        const int ready = ::poll (&p, 1, 200);
        if (ready < 0)
            return;
        if (ready == 0)
            continue;
        const auto got = ::recv (fd, chunk, sizeof (chunk), 0);
        if (got <= 0)
            return; // the client went away
        buffer.append (chunk, (size_t) got);

        for (auto nl = buffer.find ('\n'); nl != std::string::npos; nl = buffer.find ('\n'))
        {
            auto line = juce::String::fromUTF8 (buffer.data(), (int) nl).trim();
            buffer.erase (0, nl + 1);
            if (line.isEmpty())
                continue;

            // the Song belongs to the message thread: run the request there and wait for its answer
            auto answer = std::make_shared<std::promise<juce::String>>();
            auto future = answer->get_future();
            juce::MessageManager::callAsync ([this, keep = alive, line, answer] {
                answer->set_value (*keep ? handle (line) : juce::String (R"({"jsonrpc":"2.0","error":{"code":-32000,"message":"shutting down"},"id":null})"));
            });
            while (future.wait_for (std::chrono::milliseconds (100)) != std::future_status::ready)
                if (threadShouldExit())
                    return;

            const auto out = future.get().toStdString() + "\n";
            for (size_t sent = 0; sent < out.size();)
            {
                const auto n = ::send (fd, out.data() + sent, out.size() - sent, sendFlags);
                if (n <= 0)
                    return;
                sent += (size_t) n;
            }
        }
    }
}

#endif
} // namespace lynx
