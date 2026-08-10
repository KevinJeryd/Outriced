#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace oc {

// Native "choose a folder" dialog. Returns nullopt if the user cancelled.
std::optional<std::filesystem::path> pick_folder(const std::wstring& title,
                                                 const std::filesystem::path& initial);

// Stores a path in settings as a relative one when it sits under the app root,
// so a portable copy of the app keeps working after being moved or shared, and
// as an absolute path otherwise.
std::string portable_path(const std::filesystem::path& chosen,
                          const std::filesystem::path& root);

} // namespace oc
