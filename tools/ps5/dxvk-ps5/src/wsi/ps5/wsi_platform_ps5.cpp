// PS5 WSI for DXVK native (Open Horizon 2): one display, presented through
// VK_KHR_display, which PS5_Vulkan's RADV implements on VideoOut (its
// radv/radv_smoke.c: one 3840x2160 display, plane 0, opaque alpha). There are
// no windows: every HWND is the whole display.
#if defined(DXVK_WSI_PS5)

#include "../wsi_platform.h"

#include "../../util/log/log.h"
#include "../../util/util_string.h"

#include <vector>

namespace dxvk::wsi {

  // The display mode, read when the surface is created (4K until then: what
  // VideoOut reports).
  static uint32_t g_width = 3840, g_height = 2160, g_refresh_mhz = 60000;

  static HMONITOR ps5Monitor() { return reinterpret_cast<HMONITOR>(uintptr_t(1)); }

  static void fillMode(WsiMode* pMode) {
    pMode->width = g_width;
    pMode->height = g_height;
    pMode->refreshRate = WsiRational{ g_refresh_mhz, 1000 };
    pMode->bitsPerPixel = 32;
    pMode->interlaced = false;
  }

  class Ps5WsiDriver : public WsiDriver {
  public:
    std::vector<const char*> getInstanceExtensions() override {
      return { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_DISPLAY_EXTENSION_NAME };
    }

    HMONITOR getDefaultMonitor() override { return ps5Monitor(); }
    HMONITOR enumMonitors(uint32_t index) override { return index == 0 ? ps5Monitor() : nullptr; }
    HMONITOR enumMonitors(const LUID*[], uint32_t, uint32_t index) override { return enumMonitors(index); }

    bool getDisplayName(HMONITOR, WCHAR (&Name)[32]) override {
      const char* name = "\\\\.\\DISPLAY1";
      size_t i = 0;
      for (; name[i] && i < 31; i++) Name[i] = WCHAR(name[i]);
      Name[i] = 0;
      return true;
    }

    bool getDesktopCoordinates(HMONITOR, RECT* pRect) override {
      pRect->left = 0;
      pRect->top = 0;
      pRect->right = LONG(g_width);
      pRect->bottom = LONG(g_height);
      return true;
    }

    bool getDisplayMode(HMONITOR, uint32_t modeNumber, WsiMode* pMode) override {
      if (modeNumber != 0) return false;
      fillMode(pMode);
      return true;
    }
    bool getCurrentDisplayMode(HMONITOR, WsiMode* pMode) override { fillMode(pMode); return true; }
    bool getDesktopDisplayMode(HMONITOR, WsiMode* pMode) override { fillMode(pMode); return true; }
    WsiEdidData getMonitorEdid(HMONITOR) override { return {}; }

    void getWindowSize(HWND, uint32_t* pWidth, uint32_t* pHeight) override {
      if (pWidth) *pWidth = g_width;
      if (pHeight) *pHeight = g_height;
    }

    // The display is the window: nothing to move, resize or restore.
    void resizeWindow(HWND, DxvkWindowState*, uint32_t, uint32_t) override { }
    void saveWindowState(HWND, DxvkWindowState*, bool) override { }
    void restoreWindowState(HWND, DxvkWindowState*, bool) override { }
    bool setWindowMode(HMONITOR, HWND, DxvkWindowState*, const WsiMode&) override { return true; }
    bool enterFullscreenMode(HMONITOR, HWND, DxvkWindowState*, bool) override { return true; }
    bool leaveFullscreenMode(HWND, DxvkWindowState*) override { return true; }
    bool restoreDisplayMode() override { return true; }
    HMONITOR getWindowMonitor(HWND) override { return ps5Monitor(); }
    bool isWindow(HWND) override { return true; }
    bool isMinimized(HWND) override { return false; }
    bool isOccluded(HWND) override { return false; }
    void updateFullscreenWindow(HMONITOR, HWND, bool) override { }

    VkResult createSurface(HWND, PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkSurfaceKHR* pSurface) override {
      auto enumDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
      auto getDisplays = reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(gipa(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR"));
      auto getModes = reinterpret_cast<PFN_vkGetDisplayModePropertiesKHR>(gipa(instance, "vkGetDisplayModePropertiesKHR"));
      auto createPlaneSurface = reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(gipa(instance, "vkCreateDisplayPlaneSurfaceKHR"));
      if (!enumDevices || !getDisplays || !getModes || !createPlaneSurface) {
        Logger::err("PS5 WSI: VK_KHR_display entry points missing");
        return VK_ERROR_EXTENSION_NOT_PRESENT;
      }

      uint32_t count = 1;
      VkPhysicalDevice device = VK_NULL_HANDLE;
      VkResult vr = enumDevices(instance, &count, &device);
      if ((vr != VK_SUCCESS && vr != VK_INCOMPLETE) || !count) return VK_ERROR_INITIALIZATION_FAILED;

      count = 1;
      VkDisplayPropertiesKHR display = { };
      vr = getDisplays(device, &count, &display);
      if ((vr != VK_SUCCESS && vr != VK_INCOMPLETE) || !count) {
        Logger::err("PS5 WSI: no display");
        return VK_ERROR_INITIALIZATION_FAILED;
      }

      uint32_t modeCount = 0;
      getModes(device, display.display, &modeCount, nullptr);
      std::vector<VkDisplayModePropertiesKHR> modes(modeCount);
      if (!modeCount || getModes(device, display.display, &modeCount, modes.data()) < 0) {
        Logger::err("PS5 WSI: the display has no modes");
        return VK_ERROR_INITIALIZATION_FAILED;
      }
      const VkDisplayModePropertiesKHR& mode = modes[0];
      g_width = mode.parameters.visibleRegion.width;
      g_height = mode.parameters.visibleRegion.height;
      g_refresh_mhz = mode.parameters.refreshRate;
      Logger::info(str::format("PS5 WSI: ", display.displayName ? display.displayName : "display", ", ",
        g_width, "x", g_height, " @ ", g_refresh_mhz / 1000.0, " Hz (", modeCount, " mode(s))"));

      VkDisplaySurfaceCreateInfoKHR info = { VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR };
      info.displayMode = mode.displayMode;
      info.planeIndex = 0;
      info.planeStackIndex = 0;
      info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
      info.globalAlpha = 1.0f;
      info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
      info.imageExtent = mode.parameters.visibleRegion;
      return createPlaneSurface(instance, &info, nullptr, pSurface);
    }
  };

  static bool createPs5WsiDriver(WsiDriver** driver) {
    *driver = new Ps5WsiDriver();
    return true;
  }

  WsiBootstrap Ps5WSI = {
    "PS5",
    createPs5WsiDriver
  };

}

#endif
