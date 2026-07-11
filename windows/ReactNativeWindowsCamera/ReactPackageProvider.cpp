#include "pch.h"

#include "ReactPackageProvider.h"
#if __has_include("ReactPackageProvider.g.cpp")
#include "ReactPackageProvider.g.cpp"
#endif

// Registers the RNWCameraCapture attributed module (CameraCaptureModule.h) via
// AddAttributedModules. The live-preview Fabric ComponentView is a scaffold
// (scaffold/CameraViewComponentView.*) and is intentionally NOT compiled or
// registered yet — see README "Roadmap". When it lands, this file adds:
//   #include "CameraViewComponentView.h"
//   ... CameraViewComponentView::RegisterViewComponent(packageBuilder);

using namespace winrt::Microsoft::ReactNative;

namespace winrt::ReactNativeWindowsCamera::implementation
{

void ReactPackageProvider::CreatePackage(IReactPackageBuilder const &packageBuilder) noexcept
{
  AddAttributedModules(packageBuilder, true);
}

} // namespace winrt::ReactNativeWindowsCamera::implementation
