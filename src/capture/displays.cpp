#include "capture/displays.h"

#include <windows.h>
#include <dxgi1_2.h>

#include "platform/subprocess.h"

#pragma comment(lib, "dxgi.lib")

namespace oc {
namespace {

// Refresh rate is not in DXGI_OUTPUT_DESC, so pull it from the display mode.
int refresh_for(const std::wstring& device_name) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(device_name.c_str(), ENUM_CURRENT_SETTINGS, &dm))
        return (int)dm.dmDisplayFrequency;
    return 0;
}

} // namespace

std::vector<DisplayInfo> enumerate_displays() {
    std::vector<DisplayInfo> out;

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory)
        return out;

    int index = 0;
    for (UINT a = 0;; ++a) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) break;

        for (UINT o = 0;; ++o) {
            IDXGIOutput* output = nullptr;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            if (!output) break;

            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.AttachedToDesktop) {
                DisplayInfo d;
                d.index       = index++;
                d.left        = desc.DesktopCoordinates.left;
                d.top         = desc.DesktopCoordinates.top;
                d.width       = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
                d.height      = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                d.device_name = to_utf8(desc.DeviceName);
                d.refresh_hz  = refresh_for(desc.DeviceName);
                d.primary     = desc.DesktopCoordinates.left == 0 &&
                                desc.DesktopCoordinates.top  == 0;

                d.label = std::to_string(d.index) + ":  " +
                          std::to_string(d.width) + "x" + std::to_string(d.height);
                if (d.refresh_hz > 0) d.label += " @" + std::to_string(d.refresh_hz) + "Hz";
                if (d.primary)        d.label += "  (primary)";
                out.push_back(std::move(d));
            }
            output->Release();
        }
        adapter->Release();
    }

    factory->Release();
    return out;
}

} // namespace oc
