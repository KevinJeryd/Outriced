#pragma once
#include <string>
#include <vector>

namespace oc {

struct DisplayInfo {
    int  index = 0;          // matches ddagrab output_idx / vsrc_amf monitor_index
    int  left = 0;           // desktop coordinates, physical pixels
    int  top = 0;
    int  width = 0;
    int  height = 0;
    int  refresh_hz = 0;     // 0 when unknown
    bool primary = false;
    std::string device_name; // "\\.\DISPLAY1"
    std::string label;       // "0: 3840x2160 @144Hz (primary)"
};

// Enumerated through DXGI so the ordering is exactly the one the capture
// filters use. Win32's display numbering does not always agree, and
// EnumDisplaySettings reports DPI-virtualised sizes unless the process is
// DPI-aware, so neither is trustworthy for picking a capture target.
std::vector<DisplayInfo> enumerate_displays();

} // namespace oc
