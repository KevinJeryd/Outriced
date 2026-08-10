#pragma once
#include <string>
#include <vector>

namespace oc {

struct AudioDevice {
    std::string id;        // WASAPI endpoint id, stable across reboots
    std::string name;      // "Speakers (Audient iD14)"
    bool        is_default = false;
};

// Playback endpoints. Recording one of these captures what it is playing
// (loopback), which is how game and chat audio are picked up.
std::vector<AudioDevice> enumerate_output_devices();

// Recording endpoints: microphones and line inputs.
std::vector<AudioDevice> enumerate_input_devices();

// Friendly name for an endpoint id, or an empty string if it is gone. Used to
// show a saved selection that is no longer plugged in.
std::string audio_device_name(const std::string& id);

} // namespace oc
