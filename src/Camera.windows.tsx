/**
 * react-native-windows implementation of an expo-camera-compatible surface.
 *
 * The real `expo-camera` acquires a native module (ExpoCameraManager via
 * requireNativeModule) and renders a native CameraView component. Neither
 * exists on react-native-windows (New Architecture), so importing the real
 * package crashes at boot, and its `.web` implementation needs the DOM. This
 * file provides the public expo-camera API backed by REAL native Windows code:
 *
 *   1. Capture (guaranteed): the `RNWCameraCapture` C++/WinRT attributed
 *      module (windows/ReactNativeWindowsCamera/CameraCaptureModule.{h,cpp})
 *      does a headless one-shot Windows.Media.Capture photo and returns the
 *      expo CameraCapturedPicture shape.
 *   2. Live preview (scaffold, not registered yet): when the native Fabric
 *      "RNWCameraView" component (windows/.../scaffold/CameraViewComponentView)
 *      lands, <CameraView/> mounts it (a real MediaCapture-backed view) and
 *      takePictureAsync dispatches its "takePicture" command, awaiting the
 *      matching `onPictureTaken` event (correlated by requestId). Until then,
 *      or when the command/event round-trip does not complete, we fall back to
 *      the module capture (1) and render a labeled placeholder. Either way a
 *      photo is taken.
 *   3. Permissions: wired to RNWCameraCapture (request = a real MediaCapture
 *      init probe; get = sync cached status), with a granted fallback when the
 *      module is absent (OTA JS on a binary that predates it).
 *
 * API parity target: CameraView, useCameraPermissions, FlashMode, Camera,
 * PermissionStatus — the expo-camera surface a typical app imports.
 */
import React from 'react';
import {
  View,
  StyleSheet,
  Text,
  NativeModules,
  UIManager,
  findNodeHandle,
  requireNativeComponent,
  type ViewProps,
  type HostComponent,
} from 'react-native';

// --- Enums / string-union stand-ins ----------------------------------------

export const PermissionStatus = {
  GRANTED: 'granted',
  UNDETERMINED: 'undetermined',
  DENIED: 'denied',
} as const;
export type PermissionStatus =
  (typeof PermissionStatus)[keyof typeof PermissionStatus];

export type CameraType = 'front' | 'back';
export const CameraType = { front: 'front', back: 'back' } as const;

export type FlashMode = 'off' | 'on' | 'auto';
export const FlashMode = { off: 'off', on: 'on', auto: 'auto' } as const;

export type CameraMode = 'picture' | 'video';
export type FocusMode = 'on' | 'off';
export type BarcodeType = string;

// --- Permission response shape ---------------------------------------------

export interface PermissionResponse {
  status: PermissionStatus;
  granted: boolean;
  canAskAgain: boolean;
  expires: 'never' | number;
}

const GRANTED_RESPONSE: PermissionResponse = {
  status: PermissionStatus.GRANTED,
  granted: true,
  canAskAgain: true,
  expires: 'never',
};

// --- Native module binding (RNWCameraCapture) -------------------------------

interface NativePermission {
  status?: string;
  granted?: boolean;
  canAskAgain?: boolean;
  expires?: 'never' | number;
}

interface NativePicture {
  uri: string;
  width: number;
  height: number;
  base64?: string | null;
  exif?: Record<string, unknown> | null;
}

interface CameraCaptureNative {
  takePictureAsync(options: Record<string, unknown>): Promise<NativePicture | null>;
  requestCameraPermissions(): Promise<NativePermission>;
  // REACT_SYNC_METHOD — returns synchronously (not a Promise).
  getCameraPermissions(): NativePermission;
}

const RNWCameraCapture = (
  NativeModules as { RNWCameraCapture?: CameraCaptureNative } | undefined
)?.RNWCameraCapture;

function normalizePermission(
  raw: NativePermission | undefined | null,
): PermissionResponse {
  if (!raw) return GRANTED_RESPONSE;
  const status = (raw.status as PermissionStatus) ?? PermissionStatus.GRANTED;
  return {
    status,
    granted: raw.granted ?? status === PermissionStatus.GRANTED,
    canAskAgain: raw.canAskAgain ?? true,
    expires: raw.expires ?? 'never',
  };
}

// --- Permission helpers (native-backed, granted fallback) ------------------

async function getCameraPermissionsAsync(): Promise<PermissionResponse> {
  if (RNWCameraCapture?.getCameraPermissions) {
    try {
      return normalizePermission(RNWCameraCapture.getCameraPermissions());
    } catch {
      /* fall through */
    }
  }
  return GRANTED_RESPONSE;
}

async function requestCameraPermissionsAsync(): Promise<PermissionResponse> {
  if (RNWCameraCapture?.requestCameraPermissions) {
    try {
      return normalizePermission(await RNWCameraCapture.requestCameraPermissions());
    } catch {
      /* fall through */
    }
  }
  return GRANTED_RESPONSE;
}

// Microphone is unused on Windows in this package (photos only) — always granted.
async function getMicrophonePermissionsAsync(): Promise<PermissionResponse> {
  return GRANTED_RESPONSE;
}
async function requestMicrophonePermissionsAsync(): Promise<PermissionResponse> {
  return GRANTED_RESPONSE;
}

export {
  getCameraPermissionsAsync,
  requestCameraPermissionsAsync,
  getMicrophonePermissionsAsync,
  requestMicrophonePermissionsAsync,
};

type PermissionHookResult = [
  PermissionResponse,
  () => Promise<PermissionResponse>,
  () => Promise<PermissionResponse>,
];

export function useCameraPermissions(): PermissionHookResult {
  const [response, setResponse] = React.useState<PermissionResponse>(() => {
    if (RNWCameraCapture?.getCameraPermissions) {
      try {
        return normalizePermission(RNWCameraCapture.getCameraPermissions());
      } catch {
        /* fall through */
      }
    }
    return GRANTED_RESPONSE;
  });

  const request = React.useCallback(async () => {
    const r = await requestCameraPermissionsAsync();
    setResponse(r);
    return r;
  }, []);
  const get = React.useCallback(async () => {
    const r = await getCameraPermissionsAsync();
    setResponse(r);
    return r;
  }, []);

  return [response, request, get];
}

export function useMicrophonePermissions(): PermissionHookResult {
  return [
    GRANTED_RESPONSE,
    requestMicrophonePermissionsAsync,
    getMicrophonePermissionsAsync,
  ];
}

// --- Barcode scan stub -------------------------------------------------------

export async function scanFromURLAsync(
  _url: string,
  _barcodeTypes?: BarcodeType[],
): Promise<unknown[]> {
  return [];
}

// --- Picture result shape ----------------------------------------------------

export interface CameraCapturedPicture {
  uri: string;
  width: number;
  height: number;
  base64?: string;
  exif?: Record<string, unknown> | null;
}

// --- Native "RNWCameraView" Fabric component detection -----------------------
//
// The native Fabric ComponentView (scaffold/CameraViewComponentView.{h,cpp}) is
// intentionally NOT part of the build yet — the live-preview viewfinder is the
// next milestone (WinUI 3 has no CaptureElement; the viewfinder needs a
// MediaFrameReader → Composition surface → ContentIsland handoff; see README
// roadmap). Do NOT probe/resolve a native component here yet: on the New
// Architecture, UIManager.getViewManagerConfig() of an unregistered name warns,
// and requireNativeComponent of an unregistered name renders a red
// "Unimplemented component" box. <CameraView/> therefore renders a labeled
// placeholder and captures via the RNWCameraCapture module (see the ref below).
// When the ComponentView is registered, set this to
// requireNativeComponent<NativeCameraViewProps>(CAMERA_COMPONENT_NAME).

const CAMERA_COMPONENT_NAME = 'RNWCameraView';

interface NativeCameraViewProps extends ViewProps {
  facing?: CameraType;
  flash?: FlashMode;
  onPictureTaken?: (event: {
    nativeEvent: {
      requestId: string;
      uri: string;
      width: number;
      height: number;
      base64?: string | null;
      exif?: Record<string, unknown> | null;
    };
  }) => void;
}

const NativeCameraView: HostComponent<NativeCameraViewProps> | null = null;
void CAMERA_COMPONENT_NAME;
void requireNativeComponent;

// --- CameraView component + imperative ref -----------------------------------

export interface CameraViewRef {
  takePictureAsync: (
    options?: Record<string, unknown>,
  ) => Promise<CameraCapturedPicture | undefined>;
  recordAsync: (options?: Record<string, unknown>) => Promise<{ uri: string }>;
  stopRecording: () => void;
  pausePreview: () => void;
  resumePreview: () => void;
  getAvailablePictureSizesAsync: () => Promise<string[]>;
}

export interface CameraViewProps extends ViewProps {
  facing?: CameraType;
  flash?: FlashMode;
  mode?: CameraMode;
  zoom?: number;
  enableTorch?: boolean;
  onCameraReady?: () => void;
  onBarcodeScanned?: (result: unknown) => void;
  children?: React.ReactNode;
}

const TAKE_PICTURE_TIMEOUT_MS = 8000;

// Headless capture via the RNWCameraCapture module. This is the guaranteed path
// (works with or without the native preview view).
async function takePictureViaModule(
  facing: CameraType,
  options?: Record<string, unknown>,
): Promise<CameraCapturedPicture | undefined> {
  if (!RNWCameraCapture?.takePictureAsync) return undefined;
  try {
    const r = await RNWCameraCapture.takePictureAsync({
      base64: options?.base64 !== false,
      facing,
    });
    if (!r) return undefined;
    return {
      uri: r.uri,
      width: r.width,
      height: r.height,
      base64: r.base64 ?? undefined,
      exif: r.exif ?? null,
    };
  } catch (e) {
    // eslint-disable-next-line no-console
    console.warn('[react-native-windows-camera] native capture failed:', e);
    return undefined;
  }
}

export const CameraView = React.forwardRef<CameraViewRef, CameraViewProps>(
  ({ children, style, facing = 'back', flash = 'off', ...rest }, ref) => {
    // Loosely typed: holds the mounted native-component instance for findNodeHandle.
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const nativeRef = React.useRef<any>(null);
    // requestId -> resolver for an in-flight takePicture command.
    const pendingRef = React.useRef<
      Map<string, (p: CameraCapturedPicture | undefined) => void>
    >(new Map());
    const counterRef = React.useRef(0);
    // Keep latest facing available to imperative callbacks without re-binding them.
    const facingRef = React.useRef<CameraType>(facing);
    facingRef.current = facing;

    // Native onPictureTaken -> resolve the correlated pending promise.
    const handlePictureTaken = React.useCallback(
      (event: {
        nativeEvent: {
          requestId: string;
          uri: string;
          width: number;
          height: number;
          base64?: string | null;
          exif?: Record<string, unknown> | null;
        };
      }) => {
        const p = event?.nativeEvent;
        if (!p) return;
        const resolve = pendingRef.current.get(p.requestId);
        if (!resolve) return;
        pendingRef.current.delete(p.requestId);
        if (p.uri) {
          resolve({
            uri: p.uri,
            width: p.width,
            height: p.height,
            base64: p.base64 ?? undefined,
            exif: p.exif ?? null,
          });
        } else {
          // Native reported a failure — fall back to the module capture.
          void takePictureViaModule(facingRef.current).then(resolve);
        }
      },
      [],
    );

    const takePictureViaCommand = React.useCallback(
      (options?: Record<string, unknown>): Promise<CameraCapturedPicture | undefined> =>
        new Promise((resolve) => {
          const node = nativeRef.current
            ? findNodeHandle(nativeRef.current)
            : null;
          if (node == null) {
            void takePictureViaModule(facingRef.current, options).then(resolve);
            return;
          }
          const requestId = `${Date.now()}_${++counterRef.current}`;
          const wantBase64 = options?.base64 !== false;

          let settled = false;
          const finish = (p: CameraCapturedPicture | undefined) => {
            if (settled) return;
            settled = true;
            pendingRef.current.delete(requestId);
            resolve(p);
          };
          pendingRef.current.set(requestId, finish);

          try {
            (UIManager as unknown as {
              dispatchViewManagerCommand: (
                node: number,
                command: string,
                args: unknown[],
              ) => void;
            }).dispatchViewManagerCommand(node, 'takePicture', [
              requestId,
              wantBase64,
            ]);
          } catch {
            // Command dispatch not available — fall back immediately.
            void takePictureViaModule(facingRef.current, options).then(finish);
            return;
          }

          // Safety net: if the native event never arrives, fall back.
          setTimeout(() => {
            if (!settled) {
              void takePictureViaModule(facingRef.current, options).then(finish);
            }
          }, TAKE_PICTURE_TIMEOUT_MS);
        }),
      [],
    );

    React.useImperativeHandle(
      ref,
      () => ({
        takePictureAsync: async (options?: Record<string, unknown>) => {
          if (NativeCameraView && nativeRef.current) {
            return takePictureViaCommand(options);
          }
          return takePictureViaModule(facingRef.current, options);
        },
        // Video is intentionally unsupported on Windows for now (photos only).
        recordAsync: async () => ({ uri: '' }),
        stopRecording: () => {},
        pausePreview: () => {},
        resumePreview: () => {},
        getAvailablePictureSizesAsync: async () => [],
      }),
      [takePictureViaCommand],
    );

    if (NativeCameraView) {
      const NativeView = NativeCameraView;
      return (
        <NativeView
          {...(rest as ViewProps)}
          ref={nativeRef}
          style={[styles.nativeCamera, style]}
          facing={facing}
          flash={flash}
          onPictureTaken={handlePictureTaken}
          collapsable={false}>
          {children}
        </NativeView>
      );
    }

    // No native preview view registered — labeled placeholder (capture still
    // works via the RNWCameraCapture module through the ref above).
    return (
      <View
        {...(rest as ViewProps)}
        style={[styles.placeholder, style]}
        accessibilityLabel='Camera preview unavailable on Windows'>
        <Text style={styles.text}>Camera preview unavailable on Windows</Text>
        {children}
      </View>
    );
  },
);
CameraView.displayName = 'CameraView(Windows)';

// Legacy alias — older expo-camera exposed a `Camera` component too.
export { CameraView as CameraViewComponent };

// --- `Camera` namespace object (expo-camera index.ts `export const Camera`) --

export const Camera = {
  getCameraPermissionsAsync,
  requestCameraPermissionsAsync,
  getMicrophonePermissionsAsync,
  requestMicrophonePermissionsAsync,
  scanFromURLAsync,
};

const styles = StyleSheet.create({
  nativeCamera: {
    backgroundColor: '#000000',
  },
  placeholder: {
    backgroundColor: '#000000',
    alignItems: 'center',
    justifyContent: 'center',
  },
  text: {
    color: '#FFFFFF',
    fontSize: 14,
    opacity: 0.6,
  },
});

export default CameraView;
