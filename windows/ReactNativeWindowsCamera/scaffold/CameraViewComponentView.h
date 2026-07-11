#pragma once
#include "NativeModules.h"

// ============================================================================
// CameraViewComponentView — a Fabric ComponentView SCAFFOLD for a live
// <CameraView/> preview on react-native-windows (New Architecture /
// Composition).
//
// ⚠️ SCAFFOLD — EXCLUDED FROM BUILD. This file is intentionally NOT referenced
// by ReactNativeWindowsCamera.vcxproj. It compiles-quality code extracted from
// a production app's staging tree, published so the next milestone starts from
// reviewed, teardown-correct plumbing instead of a blank file. See README
// "Roadmap" before wiring it in.
//
// WHY THIS EXISTS
// ---------------
// The CameraCaptureModule in this package already provides REAL headless photo
// capture (a one-shot MediaCapture, no preview). THIS class is the
// *live-preview* half: a native UI view that owns a MediaCapture, so
// <CameraView/> shows a viewfinder and its ref.takePictureAsync() grabs a
// still from that same live session.
//
// WHY A COMPONENTVIEW, NOT A TURBOMODULE
// --------------------------------------
// A camera preview is a native UI view, so it is registered as a Fabric
// ComponentView via
//   IReactPackageBuilderFabric.AddViewComponent(name, provider)
// There is NO attributed auto-registration for view components: the package
// provider must call CameraViewComponentView::RegisterViewComponent(builder).
//
// WHAT IS DONE vs WHAT REMAINS
// ----------------------------
//   DONE (pure Windows SDK + RNW Composition, no extra NuGet):
//     - registration plumbing, facing/flash props, per-instance MediaCapture
//       init (front/back), the "takePicture" command, the "topPictureTaken"
//       event round-trip (a real still is captured from the component's live
//       MediaCapture), and — importantly — the instance-registry TEARDOWN FIX:
//       entries are erased by the view's Destroying handler, which also
//       releases the webcam and nulls the EventEmitter so an in-flight capture
//       can never dispatch into a dead JS view (an earlier iteration leaked
//       registry entries for the life of the session).
//   REMAINING (the next milestone — the actual visible viewfinder):
//     - WinUI 3 (Microsoft.UI.Xaml.Controls) does NOT port UWP's
//       CaptureElement, so there is no drop-in XAML preview control. The
//       preview must be produced via a MediaFrameReader feeding a Microsoft.UI
//       Composition surface hosted in a Microsoft.UI.Content.ContentIsland
//       handed to RNW's Composition::ContentIslandComponentView.Connect() —
//       the exact island-hosting recipe already PROVEN by our WebView2
//       component (see FacilitronWorks/react-native-webview-windows and
//       FacilitronWorks/rnw-native-core). Until that lands, the view renders
//       an empty host visual (black), which does NOT block capture: the JS
//       surface guarantees a photo via the RNWCameraCapture module.
// ============================================================================

namespace winrt::ReactNativeWindowsCamera::implementation
{
    // Public registration hook. Call ONCE from ReactPackageProvider::CreatePackage:
    //   CameraViewComponentView::RegisterViewComponent(packageBuilder);
    struct CameraViewComponentView
    {
        // Registers the "RNWCameraView" Fabric component with the RN instance.
        // The name MUST match the native-component name the JS surface mounts
        // (see src/Camera.windows.tsx -> CAMERA_COMPONENT_NAME).
        static void RegisterViewComponent(
            winrt::Microsoft::ReactNative::IReactPackageBuilder const &packageBuilder) noexcept;
    };
}
