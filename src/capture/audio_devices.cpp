#include "capture/audio_devices.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

#include "platform/subprocess.h"

namespace oc {
namespace {

// Scoped COM init that tolerates the apartment already being set by the caller.
struct ComScope {
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);
    }
    ~ComScope() { if (owned) CoUninitialize(); }
};

std::vector<AudioDevice> enumerate(EDataFlow flow) {
    ComScope com;
    std::vector<AudioDevice> out;

    IMMDeviceEnumerator* enumerator = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))) || !enumerator)
        return out;

    // Which endpoint is default, so it can be marked in the list.
    std::string default_id;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &def)) && def) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)) && id) {
            default_id = to_utf8(id);
            CoTaskMemFree(id);
        }
        def->Release();
    }

    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices)) &&
        devices) {
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(devices->Item(i, &device)) || !device) continue;

            AudioDevice entry;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id)) && id) {
                entry.id = to_utf8(id);
                CoTaskMemFree(id);
            }

            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props)) && props) {
                PROPVARIANT name;
                PropVariantInit(&name);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
                    name.vt == VT_LPWSTR && name.pwszVal) {
                    entry.name = to_utf8(name.pwszVal);
                }
                PropVariantClear(&name);
                props->Release();
            }

            if (!entry.id.empty()) {
                if (entry.name.empty()) entry.name = "(unnamed device)";
                entry.is_default = entry.id == default_id;
                out.push_back(std::move(entry));
            }
            device->Release();
        }
        devices->Release();
    }
    enumerator->Release();
    return out;
}

} // namespace

std::vector<AudioDevice> enumerate_output_devices() { return enumerate(eRender); }
std::vector<AudioDevice> enumerate_input_devices()  { return enumerate(eCapture); }

std::string audio_device_name(const std::string& id) {
    if (id.empty()) return {};
    for (const auto& d : enumerate_output_devices()) if (d.id == id) return d.name;
    for (const auto& d : enumerate_input_devices())  if (d.id == id) return d.name;
    return {};
}

} // namespace oc
