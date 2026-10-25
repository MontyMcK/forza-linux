// Shared Windows.Xbox.* singletons (the one user, the one gamepad controller).
#pragma once

#include <string>

namespace fl::winrt {

void* xbox_user();        // IUser, AddRef'd
void* xbox_controller();  // IController of the one gamepad, AddRef'd

// An attribute of the package manifest's <mx:XboxLive ...> element (TitleId,
// PrimaryServiceConfigId), empty if absent.
std::u16string xbox_live_manifest_attr(const char* key);

}  // namespace fl::winrt
