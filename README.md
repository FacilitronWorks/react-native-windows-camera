> ⚠️ **WIP — pre-release.** The capture module inside is production-proven (shipping today in a production Expo + react-native-windows 0.83 new-architecture app), but this standalone package build has NOT yet been compile-validated, and the live viewfinder is a scaffold, not a feature. Do not consume until the first tagged release.

# @facilitronworks/react-native-windows-camera

A camera for **react-native-windows New Architecture** (Fabric/Composition).

**Real today:** one-shot webcam **photo capture** via
`Windows.Media.Capture.MediaCapture` — JPEG file + pixel dimensions + base64,
front/back device selection, permission probing — behind an
**expo-camera-compatible JS surface** (`CameraView`, `useCameraPermissions`,
`Camera`, `PermissionStatus`, `FlashMode`).

**Next milestone (scaffold included):** a live-viewfinder Fabric
`ComponentView`, to be hosted in the RN composition tree with the same
XamlIsland/ContentIsland recipe our WebView component already ships in
production.

## Why this package exists

There is no camera on Windows for React Native. At all.

- `react-native-camera` — the community camera that once had a Windows
  implementation — was deprecated and archived (dead by 2023).
- Its designated successor `react-native-vision-camera` supports iOS, Android,
  and (experimentally) macOS. **No Windows support, none planned.**
- `expo-camera` has native implementations for iOS, Android, and web DOM.
  **Nothing for react-native-windows** — importing it on RNW crashes at boot
  (`requireNativeModule('ExpoCamera…')`).

Every RNW app that needs a photo has been on its own since. This package is
the extraction of what we built to solve it for a shipping product.

## Status matrix — what is real vs. pending

| Surface | Status | Notes |
| --- | --- | --- |
| `takePictureAsync()` (headless capture) | ✅ **Real, in production** | `RNWCameraCapture` module: MediaCapture one-shot JPEG into app LocalFolder; returns expo `CameraCapturedPicture` shape (`{ uri, width, height, base64?, exif: null }`) |
| `base64` option | ✅ Real | `CryptographicBuffer.EncodeToBase64String` over the JPEG bytes |
| Width/height read-back | ✅ Real | `BitmapDecoder` over the captured file (best effort) |
| `facing: 'front' \| 'back'` | ✅ Real | enclosure-panel device selection; desktop webcams usually report no panel → first enumerated device |
| Permissions (`useCameraPermissions`, request/get) | ✅ Real | request = a real `MediaCapture.InitializeAsync` probe (this is the OS consent gate on packaged apps); get = sync cached status |
| `<CameraView/>` mounts, children render, capture works via ref | ✅ Real | renders a labeled placeholder box today (no live preview) |
| **Live viewfinder** | 🚧 **Scaffold only** (`windows/…/scaffold/`, excluded from build) | props/commands/events/teardown plumbing done; the visible preview surface is the remaining work (see Roadmap) |
| Video recording (`recordAsync`) | ❌ Not implemented | resolves `{ uri: '' }`; photos only |
| Barcode scanning | ❌ Not implemented | `scanFromURLAsync` resolves `[]` |
| Flash / torch | ❌ Tracked, no-op | hardware flash is rare on webcams |
| Zoom, focus, picture sizes | ❌ Not implemented | |
| Standalone build of this repo | ⏳ **Pending** | see Build validation |

## Production story

The capture path is not a demo. It was extracted from **Facilitron FIT for
Windows** (react-native-windows 0.83.2 new-architecture, WinAppSDK 1.8, ARM64,
app version 2.3.728, July 2026), where it drives the app's batch photo-capture
flow and its `launchCameraAsync` path — real users taking real photos with the
machine's webcam, uploading them through the same JS pipeline as iOS/Android.
App-specific naming was parameterized during extraction (module name, capture
folder); the logic is verbatim.

This is an *extraction from a working production app; standalone build
validation is pending.* We do not claim this repo has been compiled as-is.

## Install (once released)

```sh
yarn add @facilitronworks/react-native-windows-camera
```

Autolinking picks up `windows/ReactNativeWindowsCamera.vcxproj` via
`react-native.config.js`. Add the **`webcam` DeviceCapability** to your app's
`Package.appxmanifest`:

```xml
<Capabilities>
  <DeviceCapability Name="webcam" />
</Capabilities>
```

### Consumption patterns

**A. Direct import (works on all platforms):**

```tsx
import { CameraView, useCameraPermissions } from '@facilitronworks/react-native-windows-camera'
// Windows -> this package's native capture (+ placeholder preview).
// iOS/Android -> transparently re-exports expo-camera.
```

**B. Metro alias (zero call-site changes):** keep importing `expo-camera`
everywhere and alias it on Windows in `metro.config.js`:

```js
resolver: {
  resolveRequest: (context, moduleName, platform) => {
    if (platform === 'windows' && moduleName === 'expo-camera') {
      return context.resolveRequest(
        context,
        '@facilitronworks/react-native-windows-camera',
        platform,
      )
    }
    return context.resolveRequest(context, moduleName, platform)
  },
}
```

(Pattern B is exactly how the production app consumes this code today.)

### Usage

```tsx
const ref = useRef<CameraViewRef>(null)
const [permission, requestPermission] = useCameraPermissions()

<CameraView ref={ref} facing="back" style={{ flex: 1 }} />

const photo = await ref.current?.takePictureAsync({ base64: true })
// -> { uri: 'file:///…/RNWCameraCaptures/….jpg', width, height, base64 }
```

Note: `takePictureAsync` works even though the preview is a placeholder — the
capture is headless (`MediaCapture` one-shot). Your users see a black box
labeled "Camera preview unavailable on Windows" where the viewfinder will be.

## Architecture

```
<CameraView/> (JS, src/Camera.windows.tsx — expo-camera-compatible surface)
  ├─ TODAY:  labeled placeholder View
  │          ref.takePictureAsync() ──▶ NativeModules.RNWCameraCapture
  │                                       └─ CameraCaptureModule (C++/WinRT)
  │                                            MediaCapture → JPEG → LocalFolder
  └─ NEXT:   Fabric component "RNWCameraView" (scaffold/CameraViewComponentView)
               ├─ per-instance MediaCapture (live session)
               ├─ "takePicture" command / topPictureTaken event (requestId-correlated)
               ├─ Tag-keyed instance registry + Destroying-hooked teardown
               │  (releases the webcam, nulls the EventEmitter — no stale
               │   dispatch into a dead JS view; this fix is already in the code)
               └─ TODO: MediaFrameReader → Composition surface → ContentIsland
                        ──Connect()──▶ RNW ContentIslandComponentView
```

The JS surface always resolves a photo: if the native view's command/event
round-trip fails or times out (8 s), it transparently falls back to the
headless module capture.

## Roadmap

1. **Standalone build validation** of `windows/ReactNativeWindowsCamera.sln`
   (the code compiles inside the production app project; the cpp-lib packaging
   here is unproven).
2. **Live viewfinder** — the one genuinely hard remaining piece. WinUI 3 does
   **not** port UWP's `CaptureElement`, so there is no drop-in XAML preview
   control. The plan, for which every ingredient is already proven elsewhere in
   our stack:
   - drive a `MediaFrameReader` off the component's live `MediaCapture`;
   - blit frames into a `Microsoft.UI.Composition` surface;
   - wrap the visual in a `Microsoft.UI.Content.ContentIsland` and hand it to
     RNW's `ContentIslandComponentView.Connect()` — **the exact island-hosting
     recipe our WebView2 component ships in production** (see
     [FacilitronWorks/react-native-webview-windows](https://github.com/FacilitronWorks/react-native-webview-windows)
     and the reusable primitive in
     [FacilitronWorks/rnw-native-core](https://github.com/FacilitronWorks/rnw-native-core)).
   RNW's `ContentIslandComponentView` then does sizing, positioning, scroll
   tracking, clipping, DPI, and focus for free.
3. Wire `scaffold/CameraViewComponentView.*` into the vcxproj + provider, flip
   `NativeCameraView` on in `src/Camera.windows.tsx`.
4. Flash/torch where hardware supports it; `getAvailablePictureSizesAsync`.
5. Video recording (MediaCapture supports it; needs API design + testing).

## Relationship to upstream

- **expo-camera**: this package deliberately mirrors its public JS API so it
  can serve as a drop-in Windows implementation via a Metro alias. If Expo ever
  ships Windows support in expo-modules-core, we would gladly upstream this as
  the `expo-camera` Windows platform implementation.
- **react-native-vision-camera**: different API family; if upstream ever wants
  a Windows backend, the native layer here (MediaCapture init/capture/teardown,
  device selection, consent probing) is the part that transfers.

## Build validation

**PENDING.** What exists and what has been proven:

- [x] `CameraCaptureModule.{h,cpp}` compiles and runs **inside a production app
      project** (RNW 0.83.2, WinAppSDK 1.8, ARM64, new-arch) — real photos
      captured and uploaded, shipping since July 2026.
- [ ] This standalone `windows/ReactNativeWindowsCamera.sln` has **not been
      built yet** (RNW cpp-lib template shape; same expected friction points as
      our webview package: projection flow under `UseExperimentalNuget`,
      property-sheet include paths, `RNW_NEW_ARCH` definition flow).
- [ ] Autolink end-to-end in a fresh RNW 0.83 app.
- [ ] x64 and x86 builds (production evidence is ARM64).
- [ ] The `scaffold/` ComponentView has never been compiled anywhere — it is
      staged, reviewed code, not a built artifact. It is excluded from the
      project on purpose.

## License

MIT.
