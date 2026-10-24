// Shared Windows.Xbox.* singletons (the one user, the one gamepad controller).
#pragma once

namespace fl::winrt {

void* xbox_user();        // IUser, AddRef'd
void* xbox_controller();  // IController of the one gamepad, AddRef'd

}  // namespace fl::winrt
