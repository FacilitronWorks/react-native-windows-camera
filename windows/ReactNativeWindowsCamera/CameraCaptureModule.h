#pragma once
#include "NativeModules.h"

using namespace winrt::Microsoft::ReactNative;

// ============================================================================
// CameraCaptureModule — a REAL react-native-windows webcam capture module.
//
// WHY: expo-camera has no native module for react-native-windows (New
// Architecture), and the community react-native-camera died in 2023 with no
// successor supporting Windows. This module backs an expo-camera-compatible JS
// surface (src/Camera.windows.tsx) with a native
// Windows.Media.Capture.MediaCapture single-shot photo capture, so an app can
// grab a still from the default webcam TODAY.
//
// SCOPE: this module is CAPTURE ONLY — a headless one-shot photo. There is NO
// live camera preview here. A real preview requires a Fabric ComponentView
// hosting a composition surface; that scaffold lives in
// scaffold/CameraViewComponentView.{h,cpp} (next milestone — see README).
// Capture does not need the preview: the JS surface guarantees a photo via
// this module regardless.
//
// ARCHITECTURE: attributed C++/WinRT module (REACT_MODULE), registered by
// AddAttributedModules(builder, true) inside ReactPackageProvider.cpp.
// Exposed to JS as NativeModules.RNWCameraCapture.
//
// REQUIRES: the `webcam` DeviceCapability in the consuming app's
// Package.appxmanifest — the first InitializeAsync triggers the OS
// camera-consent prompt.
//
// PROVENANCE: extracted from a production app (Facilitron FIT for Windows,
// RNW 0.83.2 new-arch, WinAppSDK 1.8, ARM64) where it ships today and drives
// the app's batch photo-capture flow. App-specific naming has been
// parameterized; standalone build validation of this repo is PENDING (see
// README "Build validation").
// ============================================================================

namespace winrt::ReactNativeWindowsCamera::implementation
{
    REACT_MODULE(CameraCaptureModule, L"RNWCameraCapture");
    struct CameraCaptureModule
    {
        REACT_INIT(Initialize);
        void Initialize(ReactContext const &reactContext) noexcept
        {
            m_context = reactContext;
        }

        // Async single-shot capture. Initializes the default video capture
        // device, captures one JPEG still into the app's LocalFolder, and
        // resolves an expo-camera CameraCapturedPicture-shaped object:
        //   { uri, width, height, base64?, exif: null }
        // `options` mirrors expo-camera's takePictureAsync options; `base64`
        // (bool) and `facing` ('front'|'back') are honored today. Rejects
        // (with a message) on any failure (no camera, consent denied, capture
        // error).
        REACT_METHOD(takePictureAsync);
        void takePictureAsync(JSValueObject options, ReactPromise<JSValue> promise) noexcept;

        // Async permission request. There is no separate photo-permission prompt
        // on a desktop packaged app beyond the `webcam` DeviceCapability, so we
        // use a real MediaCapture::InitializeAsync as the permission signal:
        // success -> granted (and the OS consent prompt, if any, surfaces here);
        // failure -> denied (canAskAgain=false only on E_ACCESSDENIED). The
        // probe capture is opened video-only (no microphone) and Close()d
        // immediately. Resolves an expo-camera PermissionResponse:
        //   { status:'granted'|'denied', granted, canAskAgain, expires:'never' }
        // The resolved status is also cached for the synchronous getter below.
        REACT_METHOD(requestCameraPermissions);
        void requestCameraPermissions(ReactPromise<JSValue> promise) noexcept;

        // Synchronous getter. We cannot probe camera consent synchronously (that
        // needs an async InitializeAsync), so this returns the LAST cached
        // status (from a prior requestCameraPermissions / takePictureAsync),
        // defaulting to 'undetermined' before any capture attempt. Same
        // PermissionResponse shape. Sync (REACT_SYNC_METHOD) so a JS
        // useCameraPermissions() hook can seed its initial state without an
        // await. Returns a JSValue object.
        REACT_SYNC_METHOD(getCameraPermissions);
        JSValue getCameraPermissions() noexcept;

    private:
        ReactContext m_context{nullptr};
    };
}
