/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

// The viewer's microphone on macOS: CoreAudio's HAL output unit, playing into
// the output side of a loopback driver — "BlackHole 2ch" — whose input side is
// the microphone the host's apps record. macOS has no virtual microphone of
// its own, and MoonlightWeb ships no audio driver.
//
// The unit is the plain HAL one (kAudioUnitSubType_HALOutput), never the
// default-output one: it plays into the device it is given by ID and follows
// nothing. A new unit is bound to the Mac's default output until it is told
// otherwise, so the device is set, read back and compared before the unit is
// initialised, and again before it starts; anything else is a refusal. There
// is no fallback: without the device the session has no microphone, rather
// than a voice coming out of the speakers. A device that leaves mid-session
// (the driver removed, coreaudiod restarted) just stops asking for sound; the
// unit is never pointed anywhere else, and the next session looks it up again.
//
// Which device:
//   - empty or "auto": the one named exactly "BlackHole 2ch", and only if
//     CoreAudio reports it as a virtual device — an aggregate or multi-output
//     device carrying the name could end in real speakers;
//   - settings.json mic_device: that name in full, any case, else the one
//     output device whose name contains it; several is a refusal that lists
//     them, never a guess.
// Either way never this Mac's default output, unless it is a loopback driver:
// that is not a speaker, and on a Mac with no sound hardware (a VM) it is the
// only output there is. It is then used with a warning, since every app's
// sound reaches the microphone along with the voice.
//
// The ring's 48 kHz mono goes into every channel of the device. The unit
// converts the rate when the device runs at another, so the device's own
// settings, which the recording apps share, are left alone.

#include "MicCable.h"
#include "MicJitter.h"
#include "MicOutput.h"

#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mw::mic {
namespace {

/// What the automatic choice takes, by its full name and nothing near it.
constexpr const char* kBlackHole = "BlackHole 2ch";
constexpr double kRate = 48000.0;
/// One pull from the ring; a longer slice is filled in several.
constexpr size_t kScratchFrames = 4096;
constexpr UInt32 kSampleBytes = sizeof(Float32);

AudioObjectPropertyAddress address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    return {selector, scope, kAudioObjectPropertyElementMain};
}

template <typename T>
bool readValue(AudioObjectID object, const AudioObjectPropertyAddress& a, T& value)
{
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &a, 0, nullptr, &size, &value) == noErr;
}

std::string utf8(CFStringRef text)
{
    if (!text) return {};
    const CFIndex capacity =
        CFStringGetMaximumSizeForEncoding(CFStringGetLength(text), kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<size_t>(capacity), '\0');
    if (!CFStringGetCString(text, out.data(), capacity, kCFStringEncodingUTF8)) return {};
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string stringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
{
    CFStringRef value = nullptr;
    if (!readValue(object, address(selector), value) || !value) return {};
    std::string out = utf8(value);
    CFRelease(value);
    return out;
}

std::vector<AudioDeviceID> allDevices()
{
    const AudioObjectPropertyAddress a = address(kAudioHardwarePropertyDevices);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr)
        return {};
    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    if (devices.empty()) return devices;
    size = static_cast<UInt32>(devices.size() * sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size,
                                   devices.data()) != noErr)
        return {};
    devices.resize(size / sizeof(AudioDeviceID));
    return devices;
}

/// How many channels a device plays; zero for one that only records.
UInt32 outputChannels(AudioDeviceID device)
{
    const AudioObjectPropertyAddress a =
        address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &a, 0, nullptr, &size) != noErr ||
        size < offsetof(AudioBufferList, mBuffers))
        return 0;
    // Whole AudioBuffers, so the list is aligned as one.
    std::vector<AudioBuffer> storage(size / sizeof(AudioBuffer) + 1);
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, list) != noErr ||
        size < offsetof(AudioBufferList, mBuffers))
        return 0;
    const size_t held = (size - offsetof(AudioBufferList, mBuffers)) / sizeof(AudioBuffer);
    UInt32 channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers && i < held; ++i)
        channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

/// A driver with no hardware behind it, as BlackHole reports itself.
bool isVirtual(AudioDeviceID device)
{
    UInt32 transport = kAudioDeviceTransportTypeUnknown;
    return readValue(device, address(kAudioDevicePropertyTransportType), transport) &&
           transport == kAudioDeviceTransportTypeVirtual;
}

AudioDeviceID defaultDevice(AudioObjectPropertySelector selector)
{
    AudioDeviceID device = kAudioObjectUnknown;
    return readValue(kAudioObjectSystemObject, address(selector), device) ? device
                                                                          : kAudioObjectUnknown;
}

/// Muted or turned all the way down in macOS, where the device says. A
/// loopback driver applies both to what it hands on.
bool silenced(AudioDeviceID device)
{
    UInt32 mute = 0;
    if (readValue(device, address(kAudioDevicePropertyMute, kAudioObjectPropertyScopeOutput),
                  mute) &&
        mute != 0)
        return true;
    Float32 volume = 1.0f;
    return readValue(device,
                     address(kAudioDevicePropertyVolumeScalar, kAudioObjectPropertyScopeOutput),
                     volume) &&
           volume <= 0.0001f;
}

std::string quoted(const std::string& name)
{
    return "\"" + name + "\"";
}

/// A CoreAudio status as it is looked up: its four characters when it has
/// them, the number otherwise.
std::string code(OSStatus status)
{
    const auto value = static_cast<uint32_t>(status);
    const char text[5] = {static_cast<char>((value >> 24) & 0xff),
                          static_cast<char>((value >> 16) & 0xff),
                          static_cast<char>((value >> 8) & 0xff), static_cast<char>(value & 0xff),
                          '\0'};
    const bool printable =
        std::all_of(text, text + 4, [](char c) { return c >= 0x20 && c < 0x7f; });
    return printable ? "CoreAudio error '" + std::string(text) + "'"
                     : "CoreAudio error " + std::to_string(status);
}

struct Target
{
    AudioDeviceID id = kAudioObjectUnknown;
    std::string name;
    UInt32 channels = 0;
    /// Also where this Mac's own sound goes: a loopback driver only.
    bool isDefaultOutput = false;
};

/// The device to play into (the top of this file says which). Empty on
/// success, else why not, in a sentence the viewer is shown.
std::string findDevice(const std::string& device, Target& out)
{
    const bool automatic = wantsAutoDevice(device);
    const std::string wanted = lowerAscii(automatic ? std::string(kBlackHole) : device);

    std::vector<Target> exact;
    std::vector<Target> partial;
    for (AudioDeviceID id : allDevices()) {
        const std::string name = stringProperty(id, kAudioObjectPropertyName);
        const std::string lower = lowerAscii(name);
        const bool whole = lower == wanted;
        if (!whole && (automatic || lower.find(wanted) == std::string::npos)) continue;
        const UInt32 channels = outputChannels(id);
        if (channels == 0) continue; // a microphone: nothing to play into
        (whole ? exact : partial).push_back({id, name, channels, false});
    }

    if (exact.empty() && partial.size() > 1) {
        std::string names;
        for (const Target& t : partial)
            names += (names.empty() ? "" : ", ") + quoted(t.name);
        return quoted(device) +
               " (settings.json mic_device) matches several output devices on this Mac (" + names +
               "): give the full name of one";
    }
    std::vector<Target>& pool = exact.empty() ? partial : exact;
    if (pool.empty()) {
        if (!automatic)
            return "no output device named like " + quoted(device) +
                   " (settings.json mic_device) is present on this Mac";
        return "this Mac has no " + quoted(kBlackHole) +
               " device to play the microphone into. Install BlackHole 2ch "
               "(existential.audio/blackhole): MoonlightWeb plays into it, and apps record " +
               quoted(kBlackHole) + " as their microphone";
    }

    const AudioDeviceID defaultOutput = defaultDevice(kAudioHardwarePropertyDefaultOutputDevice);
    const AudioDeviceID systemOutput =
        defaultDevice(kAudioHardwarePropertyDefaultSystemOutputDevice);
    std::string refusal;
    for (Target& candidate : pool) {
        const bool loopback = isVirtual(candidate.id);
        candidate.isDefaultOutput = candidate.id == defaultOutput || candidate.id == systemOutput;
        std::string why;
        if (automatic && !loopback)
            why = quoted(candidate.name) +
                  " on this Mac is not the BlackHole driver's device (CoreAudio does not report "
                  "it as a virtual one), so it is not taken automatically. Name the device to "
                  "play into in settings.json mic_device";
        else if (candidate.isDefaultOutput && !loopback)
            why = quoted(candidate.name) +
                  " is this Mac's default output: the microphone would come out of the speakers. "
                  "Name a virtual audio device, such as BlackHole 2ch, in settings.json "
                  "mic_device";
        if (why.empty()) {
            out = candidate;
            return {};
        }
        if (refusal.empty()) refusal = why;
    }
    return refusal;
}

/// What the unit is fed: 48 kHz float, one buffer per channel.
AudioStreamBasicDescription clientFormat(UInt32 channels)
{
    AudioStreamBasicDescription format{};
    format.mSampleRate = kRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved;
    format.mFramesPerPacket = 1;
    format.mChannelsPerFrame = channels;
    format.mBitsPerChannel = 8 * kSampleBytes;
    // Per channel, since each has a buffer of its own.
    format.mBytesPerFrame = kSampleBytes;
    format.mBytesPerPacket = kSampleBytes;
    return format;
}

class CoreAudioOutput final : public IMicOutput
{
public:
    CoreAudioOutput(std::string device, LogFn log)
        : m_Device(std::move(device))
        , m_Log(std::move(log))
        , m_Mono(kScratchFrames)
    {}
    ~CoreAudioOutput() override { stop(); }

    bool start(JitterRing& ring, std::string& why) override
    {
        if (m_Unit) return true;
        Target target;
        why = findDevice(m_Device, target);
        if (!why.empty()) return false;

        m_Ring = &ring;
        why = open(target);
        if (!why.empty()) {
            close();
            return false;
        }
        // Written before start() returns and never again, so deviceName()
        // reads it without a lock.
        m_Name = target.name;
        report(target);
        return true;
    }

    void stop() override { close(); }

    std::string deviceName() const override { return m_Name; }

private:
    /// Whether the unit plays into `wanted`, as the unit itself says.
    bool boundTo(AudioDeviceID wanted) const
    {
        AudioDeviceID current = kAudioObjectUnknown;
        UInt32 size = sizeof(current);
        return AudioUnitGetProperty(m_Unit, kAudioOutputUnitProperty_CurrentDevice,
                                    kAudioUnitScope_Global, 0, &current, &size) == noErr &&
               current == wanted;
    }

    /// Empty once the unit is running into the target, else why not. The
    /// caller closes whatever a failure left behind.
    std::string open(const Target& target)
    {
        const std::string name = quoted(target.name);

        AudioComponentDescription description{};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_HALOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &description);
        if (!component) return "CoreAudio has no output unit on this Mac";
        OSStatus status = AudioComponentInstanceNew(component, &m_Unit);
        if (status != noErr || !m_Unit) {
            m_Unit = nullptr;
            return "CoreAudio could not create an output unit (" + code(status) + ")";
        }

        // Playback only, said before the device is: the unit reads both when
        // it takes one. The input side is off by default; a refusal to hear
        // that again changes nothing.
        const UInt32 off = 0;
        const UInt32 on = 1;
        AudioUnitSetProperty(m_Unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1,
                             &off, sizeof(off));
        status = AudioUnitSetProperty(m_Unit, kAudioOutputUnitProperty_EnableIO,
                                      kAudioUnitScope_Output, 0, &on, sizeof(on));
        if (status != noErr)
            return "CoreAudio's output unit refused playback (" + code(status) + ")";

        // Until here the unit holds the default output. Nothing below runs
        // unless it now holds the target instead.
        status = AudioUnitSetProperty(m_Unit, kAudioOutputUnitProperty_CurrentDevice,
                                      kAudioUnitScope_Global, 0, &target.id, sizeof(target.id));
        if (status != noErr) return "could not open " + name + " (" + code(status) + ")";
        if (!boundTo(target.id))
            return name + " could not be selected: CoreAudio kept another output device";

        // The device's own channel count, as the unit sees it; then stereo,
        // then mono, should it refuse that (the unit maps the first channels).
        AudioStreamBasicDescription hardware{};
        UInt32 size = sizeof(hardware);
        const bool known = AudioUnitGetProperty(m_Unit, kAudioUnitProperty_StreamFormat,
                                                kAudioUnitScope_Output, 0, &hardware,
                                                &size) == noErr &&
                           hardware.mChannelsPerFrame > 0;
        const UInt32 channels = known ? hardware.mChannelsPerFrame : target.channels;
        m_Channels = 0;
        UInt32 tried = 0;
        for (UInt32 count : {channels, UInt32{2}, UInt32{1}}) {
            if (count > channels || count == tried) continue;
            tried = count;
            const AudioStreamBasicDescription format = clientFormat(count);
            status = AudioUnitSetProperty(m_Unit, kAudioUnitProperty_StreamFormat,
                                          kAudioUnitScope_Input, 0, &format, sizeof(format));
            if (status == noErr) {
                m_Channels = count;
                break;
            }
        }
        if (m_Channels == 0) return name + " refused the stream's format (" + code(status) + ")";

        AURenderCallbackStruct callback{};
        callback.inputProc = &CoreAudioOutput::render;
        callback.inputProcRefCon = this;
        status = AudioUnitSetProperty(m_Unit, kAudioUnitProperty_SetRenderCallback,
                                      kAudioUnitScope_Input, 0, &callback, sizeof(callback));
        if (status != noErr) return name + " could not be fed (" + code(status) + ")";

        status = AudioUnitInitialize(m_Unit);
        if (status != noErr) return name + " could not be opened (" + code(status) + ")";
        m_Initialized = true;
        if (!boundTo(target.id))
            return name + " could not be kept: CoreAudio moved to another output device";

        status = AudioOutputUnitStart(m_Unit);
        if (status != noErr) return name + " could not be started (" + code(status) + ")";
        m_Started = true;
        return {};
    }

    /// Stops the unit before anything is torn down: once AudioOutputUnitStop
    /// returns, CoreAudio's thread is out of render() and the ring.
    void close()
    {
        if (m_Unit) {
            if (m_Started) AudioOutputUnitStop(m_Unit);
            if (m_Initialized) AudioUnitUninitialize(m_Unit);
            AudioComponentInstanceDispose(m_Unit);
        }
        m_Unit = nullptr;
        m_Started = false;
        m_Initialized = false;
        m_Ring = nullptr;
    }

    void report(const Target& target) const
    {
        if (!m_Log) return;
        const std::string name = quoted(target.name);
        std::string line = name;
        const std::string uid = stringProperty(target.id, kAudioDevicePropertyDeviceUID);
        if (!uid.empty()) line += " (" + uid + ")";
        line += ": mono into " + std::to_string(m_Channels) +
                (m_Channels == 1 ? " channel" : " channels");
        if (m_Channels < target.channels) line += " of " + std::to_string(target.channels);
        Float64 rate = 0;
        if (readValue(target.id, address(kAudioDevicePropertyNominalSampleRate), rate) &&
            rate > 0 && rate != kRate)
            line += ", 48000 Hz converted by CoreAudio to the device's " +
                    std::to_string(static_cast<long>(rate + 0.5)) + " Hz";
        else
            line += " at 48000 Hz";
        m_Log(line);

        if (target.isDefaultOutput)
            m_Log(name + " is also this Mac's default output: every app's sound reaches the "
                         "microphone with the voice. Choose another output in System Settings > "
                         "Sound where the Mac has one");
        if (silenced(target.id))
            m_Log(name + " is muted or at zero volume in macOS: apps recording it hear silence "
                         "until it is turned up (Audio MIDI Setup)");
    }

    static OSStatus render(void* context, AudioUnitRenderActionFlags* /*flags*/,
                           const AudioTimeStamp* /*when*/, UInt32 /*bus*/, UInt32 frames,
                           AudioBufferList* data)
    {
        if (data) static_cast<CoreAudioOutput*>(context)->fill(frames, *data);
        return noErr;
    }

    /// One slice, on CoreAudio's own thread: nothing here allocates. The
    /// ring's mono goes into every channel of every buffer the unit hands
    /// over, one per channel as asked or interleaved, each for as many frames
    /// as it has room for.
    void fill(UInt32 frames, AudioBufferList& data)
    {
        const auto scratch = static_cast<UInt32>(m_Mono.size());
        for (UInt32 done = 0; done < frames;) {
            const UInt32 n = std::min(frames - done, scratch);
            if (m_Ring)
                m_Ring->pull(m_Mono.data(), n);
            else
                std::fill_n(m_Mono.begin(), n, 0.0f);
            for (UInt32 b = 0; b < data.mNumberBuffers; ++b) {
                AudioBuffer& buffer = data.mBuffers[b];
                const UInt32 channels = std::max<UInt32>(buffer.mNumberChannels, 1);
                const UInt32 room = buffer.mDataByteSize / (channels * kSampleBytes);
                if (!buffer.mData || done >= room) continue;
                const UInt32 take = std::min(n, room - done);
                auto* out = static_cast<Float32*>(buffer.mData) + size_t(done) * channels;
                for (UInt32 i = 0; i < take; ++i)
                    for (UInt32 c = 0; c < channels; ++c)
                        *out++ = m_Mono[i];
            }
            done += n;
        }
    }

    const std::string m_Device;
    const LogFn m_Log;
    JitterRing* m_Ring = nullptr;
    AudioUnit m_Unit = nullptr;
    bool m_Initialized = false;
    bool m_Started = false;
    UInt32 m_Channels = 0;
    std::string m_Name;
    /// render()'s own, sized once: CoreAudio's thread is the only one in it.
    std::vector<float> m_Mono;
};

} // namespace

std::unique_ptr<IMicOutput> makeOutput(const std::string& device, LogFn log)
{
    return std::make_unique<CoreAudioOutput>(device, std::move(log));
}

std::string unavailable(const std::string& device)
{
    Target found;
    return findDevice(device, found);
}

} // namespace mw::mic
