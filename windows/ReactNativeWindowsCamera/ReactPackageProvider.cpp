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

// Required for AddAttributedModules (declared in Microsoft.ReactNative.Cxx's
// ModuleRegistration.h, which NativeModules.h pulls in). Neither pch.h nor
// ReactPackageProvider.h includes it, so without this the only declaration is
// missing and the build fails with C3861 'AddAttributedModules': identifier
// not found. Including the module header — rather than NativeModules.h
// directly — matches the sibling packages and keeps the REACT_MODULE
// registration this function depends on visible in the same translation unit.
#include "CameraCaptureModule.h"

using namespace winrt::Microsoft::ReactNative;

namespace winrt::ReactNativeWindowsCamera::implementation
{

void ReactPackageProvider::CreatePackage(IReactPackageBuilder const &packageBuilder) noexcept
{
  AddAttributedModules(packageBuilder, true);
}

} // namespace winrt::ReactNativeWindowsCamera::implementation
