#include "RigConfig.h"

#include <algorithm>
#include <cctype>

namespace fbkt
{
namespace
{
void err (std::vector<ConfigIssue>& out, const char* field, std::string message)
{
    out.push_back ({ ConfigIssue::Severity::error, field, std::move (message) });
}

void warn (std::vector<ConfigIssue>& out, const char* field, std::string message)
{
    out.push_back ({ ConfigIssue::Severity::warning, field, std::move (message) });
}

// Enough of an IPv4 check to catch a typo or an empty box. Deliberately not a
// hostname resolver: the console is on a wired local network by design, and a
// configuration that needs DNS to find it is a configuration that can lose it.
bool looksLikeIpV4 (const std::string& s)
{
    int octets = 0, digits = 0, value = 0;
    for (size_t i = 0; i <= s.size(); ++i)
    {
        if (i == s.size() || s[i] == '.')
        {
            if (digits == 0 || digits > 3 || value > 255)
                return false;
            ++octets;
            digits = 0;
            value = 0;
            continue;
        }
        if (! std::isdigit (static_cast<unsigned char> (s[i])))
            return false;
        value = value * 10 + (s[i] - '0');
        ++digits;
    }
    return octets == 4;
}
} // namespace

std::vector<ConfigIssue> validate (const RigConfig& config)
{
    std::vector<ConfigIssue> issues;
    const auto& lim = config.limits;

    // --- the four assignments ---
    if (! config.speechOutput.isValid())
        err (issues, "speechOutput", "No speech output assigned. This feeds the speaker in front of the vocal microphone.");

    if (config.consoleAddress.empty())
        err (issues, "consoleAddress", "No mixer IP address set.");
    else if (! looksLikeIpV4 (config.consoleAddress))
        err (issues, "consoleAddress", "Mixer address '" + config.consoleAddress + "' is not an IPv4 address.");

    if (config.consolePort <= 0 || config.consolePort > 65535)
        err (issues, "consolePort", "Mixer port is out of range.");

    if (! config.vocalChannel.isValid())
        err (issues, "vocalChannel", "No vocal channel assigned. This is the microphone the rig will drive into feedback.");

    if (! config.listeningMicInput.isValid())
        err (issues, "listeningMicInput", "No listening mic input assigned. Without it the rig has no way to observe the room, and no safety limit can be enforced.");

    if (! config.vocalReturnInput.isValid())
        warn (issues, "vocalReturnInput", "No vocal return input assigned. The rig can run, but it can only observe the room and not the channel itself, which limits what can be measured.");

    // Two assignments pointing at the same physical port is a wiring mistake that
    // produces plausible-looking but meaningless data, so it is an error rather
    // than a warning.
    if (config.listeningMicInput.isValid() && config.vocalReturnInput.isValid()
        && config.listeningMicInput.index == config.vocalReturnInput.index)
        err (issues, "listeningMicInput", "Listening mic and vocal return are assigned to the same input.");

    if (config.console == ConsoleModel::x32)
        err (issues, "console", "X32 support is not implemented yet.");

    if (config.gainControl == RigConfig::GainControl::sendLevel
        && (config.sendBus < 1 || config.sendBus > 16))
        err (issues, "sendBus", "Send-level gain control needs a valid bus number.");

    if (config.gainControl == RigConfig::GainControl::headAmp)
        warn (issues, "gainControl", "Head amp gain changes the level arriving at the plugin insert as well as the loop gain, so the two cannot be measured independently. The fader is normally the right choice.");

    if (config.audioDeviceName.empty())
        err (issues, "audioDeviceName", "No audio device selected.");

    if (config.sampleRate < 44100.0 || config.sampleRate > 96000.0)
        err (issues, "sampleRate", "Sample rate is outside the supported range.");

    // --- calibration ---
    if (! config.micCalibration.valid)
        warn (issues, "micCalibration", "Measurement microphone is not calibrated. The rig can measure and play, but it will not be permitted to raise gain.");

    // --- limits ---
    // These are checked for internal consistency rather than against any absolute
    // notion of safe, because what is safe depends on the boxes. What we can say
    // is that a set of limits which contradict each other cannot be enforced.
    if (lim.warnSplDb >= lim.ceilingSplDb)
        err (issues, "limits.warnSplDb", "Warning level must be below the ceiling, or there is no headroom to stop in.");

    if (lim.ceilingSplDb - lim.warnSplDb < 3.0f)
        warn (issues, "limits.warnSplDb", "Less than 3 dB between the warning level and the ceiling. Console fader smoothing alone can cover that, so the ceiling may be crossed before a pullback takes effect.");

    if (lim.ceilingSplDb > 110.0f)
        warn (issues, "limits.ceilingSplDb", "Ceiling above 110 dB SPL at the measurement microphone. Sustained feedback at that level is a thermal risk to a compression driver.");

    if (lim.maxStepDb <= 0.0f || lim.maxStepDb > 3.0f)
        err (issues, "limits.maxStepDb", "Step size must be positive and no more than 3 dB.");

    if (lim.maxGainAboveStartDb <= 0.0f)
        err (issues, "limits.maxGainAboveStartDb", "Gain ceiling above the starting point must be positive.");

    if (lim.maxBurstMs <= 0 || lim.maxBurstMs > 2000)
        err (issues, "limits.maxBurstMs", "Burst length must be positive and no more than 2000 ms.");

    if (lim.minCooldownMs < lim.maxBurstMs)
        err (issues, "limits.minCooldownMs", "Cooldown must be at least as long as the burst it follows.");

    if (lim.minCooldownMs < 4 * lim.maxBurstMs)
        warn (issues, "limits.minCooldownMs", "Cooldown is less than four times the burst length. Sustained bursts at that duty cycle will heat a compression driver faster than the thermal model assumes.");

    if (lim.watchdogTimeoutMs <= 0 || lim.watchdogTimeoutMs > 5000)
        err (issues, "limits.watchdogTimeoutMs", "Watchdog timeout must be positive and no more than 5000 ms.");

    if (lim.consoleAckTimeoutMs <= 0)
        err (issues, "limits.consoleAckTimeoutMs", "Console acknowledgement timeout must be positive.");

    if (lim.thermalTimeConstantS <= 0.0f || lim.thermalBudgetS <= 0.0f)
        err (issues, "limits.thermalBudgetS", "Thermal model needs a positive time constant and budget.");

    if (lim.normalMarginDb < 0.0f)
        err (issues, "limits.normalMarginDb", "Normal running margin cannot be negative.");

    return issues;
}

bool canRun (const std::vector<ConfigIssue>& issues) noexcept
{
    return std::none_of (issues.begin(), issues.end(), [] (const ConfigIssue& i)
                         { return i.severity == ConfigIssue::Severity::error; });
}

bool canRaiseGain (const RigConfig& config)
{
    return canRun (validate (config)) && config.micCalibration.valid;
}
} // namespace fbkt
