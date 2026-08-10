#include "app/dialogs.h"

#include <windows.h>
#include <shobjidl.h>

#include "platform/subprocess.h"

namespace oc {

std::optional<std::filesystem::path> pick_folder(const std::wstring& title,
                                                 const std::filesystem::path& initial) {
    // The dialog needs an STA apartment; the app initialises COM as MTA on the
    // audio thread, so ask for STA here and tolerate it already being set.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool owned = SUCCEEDED(init);

    std::optional<std::filesystem::path> result;

    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog))) && dialog) {
        DWORD flags = 0;
        if (SUCCEEDED(dialog->GetOptions(&flags)))
            dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                               FOS_PATHMUSTEXIST);
        if (!title.empty()) dialog->SetTitle(title.c_str());

        std::error_code ec;
        if (!initial.empty() && std::filesystem::exists(initial, ec)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.wstring().c_str(), nullptr,
                                                      IID_PPV_ARGS(&item))) && item) {
                dialog->SetFolder(item);
                item->Release();
            }
        }

        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (owned) CoUninitialize();
    return result;
}

std::string portable_path(const std::filesystem::path& chosen,
                          const std::filesystem::path& root) {
    std::error_code ec;
    const auto rel = std::filesystem::relative(chosen, root, ec);
    // Anything that has to climb out of the app root is better stored absolute;
    // a relative "..\..\somewhere" would follow the app if it were moved.
    if (!ec && !rel.empty() && rel.native().rfind(L"..", 0) != 0)
        return rel.generic_string();
    return chosen.string();
}

} // namespace oc
