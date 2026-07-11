#include "pch.h"
#include "CameraCaptureModule.h"

// WinRT projections used by the capture implementation. Pulled in here (not in
// pch.h) to keep the module self-contained. ALL of these are Windows SDK
// projections (generated under "Generated Files/winrt/") — there is NO extra
// NuGet needed for this module.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Capture.h>          // MediaCapture, *InitializationSettings, StreamingCaptureMode, PhotoCaptureSource
#include <winrt/Windows.Media.MediaProperties.h>  // ImageEncodingProperties::CreateJpeg
#include <winrt/Windows.Devices.Enumeration.h>    // DeviceInformation, DeviceClass, Panel, EnclosureLocation
#include <winrt/Windows.Storage.h>                // ApplicationData, StorageFolder/File
#include <winrt/Windows.Storage.Streams.h>        // FileIO / buffers
#include <winrt/Windows.Graphics.Imaging.h>       // BitmapDecoder (read-back width/height)
#include <winrt/Windows.Security.Cryptography.h>  // EncodeToBase64String

#include <atomic>
#include <coroutine>
#include <string>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Security::Cryptography;

namespace
{
    // Cached last-known permission status shared by the async request + sync getter.
    // 0 = undetermined, 1 = granted, 2 = denied. Written from the capture/probe
    // coroutines (any thread) and read from the sync getter, hence atomic.
    std::atomic<int> g_lastPermStatus{0};

    // Awaiter that resumes the coroutine on an IReactDispatcher's thread. Used to
    // hop onto the RNW UI thread before touching MediaCapture.
    struct ResumeOnDispatcher
    {
        winrt::Microsoft::ReactNative::IReactDispatcher dispatcher;

        bool await_ready() const noexcept
        {
            return dispatcher && dispatcher.HasThreadAccess();
        }
        void await_suspend(std::coroutine_handle<> resume) const
        {
            if (!dispatcher)
            {
                // Instance tearing down / dispatcher not attached: resuming inline on
                // the current thread beats a guaranteed null-vtable call.
                resume();
                return;
            }
            dispatcher.Post([resume]() noexcept { resume(); });
        }
        void await_resume() const noexcept {}
    };

    // Build a percent-encoded file:/// URI from a Win32 path. LocalFolder lives
    // under the user profile, which can contain spaces / non-ASCII; RN <Image>
    // and downstream consumers all take this uri.
    std::string MakeFileUri(winrt::hstring const &path)
    {
        static constexpr char kHex[] = "0123456789ABCDEF";
        const std::string utf8 = winrt::to_string(path);
        std::string uri = "file:///";
        uri.reserve(utf8.size() + 8);
        for (char raw : utf8)
        {
            const unsigned char ch = static_cast<unsigned char>(raw);
            if (ch == '\\')
            {
                uri.push_back('/');
            }
            else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9') || ch == '/' || ch == ':' ||
                     ch == '.' || ch == '-' || ch == '_' || ch == '~')
            {
                uri.push_back(static_cast<char>(ch));
            }
            else
            {
                uri.push_back('%');
                uri.push_back(kHex[ch >> 4]);
                uri.push_back(kHex[ch & 0x0F]);
            }
        }
        return uri;
    }

    // Unique "<guid>.jpg" filename for a capture. We also pass GenerateUniqueName
    // to CreateFileAsync as belt-and-suspenders.
    winrt::hstring NewGuidJpgName()
    {
        GUID g{};
        (void)::CoCreateGuid(&g);
        wchar_t buf[64] = {};
        // {XXXXXXXX-XXXX-...}; strip the surrounding braces for a clean file name.
        int n = ::StringFromGUID2(g, buf, static_cast<int>(std::size(buf)));
        std::wstring s = (n > 2) ? std::wstring(buf + 1, buf + (n - 2)) : std::wstring(buf);
        s.append(L".jpg");
        return winrt::hstring{s};
    }

    JSValueObject MakePermissionObject(int status, bool canAskAgain)
    {
        JSValueObject o;
        o["status"] = status == 1 ? "granted" : (status == 2 ? "denied" : "undetermined");
        o["granted"] = (status == 1);
        o["canAskAgain"] = canAskAgain;
        o["expires"] = "never";
        return o;
    }

    // Pick the video capture DeviceInformation.Id whose enclosure panel matches
    // the requested facing. Most desktop webcams report Panel::Unknown / null
    // enclosure, so this falls back to the first enumerated device (empty =>
    // MediaCapture default).
    IAsyncOperation<winrt::hstring> PickDeviceIdAsync(std::wstring facing)
    {
        const Panel wanted = (facing == L"front") ? Panel::Front : Panel::Back;
        winrt::hstring fallback;
        try
        {
            auto devices = co_await DeviceInformation::FindAllAsync(DeviceClass::VideoCapture);
            for (auto const &d : devices)
            {
                if (fallback.empty())
                {
                    fallback = d.Id();
                }
                auto loc = d.EnclosureLocation();
                if (loc && loc.Panel() == wanted)
                {
                    co_return d.Id();
                }
            }
        }
        catch (...)
        {
            // fall through to fallback (possibly empty)
        }
        co_return fallback;
    }

    // The capture coroutine. Free fire_and_forget (not a member) so it owns
    // everything by value and does not depend on module lifetime across
    // suspension points.
    winrt::fire_and_forget CaptureAsync(
        winrt::Microsoft::ReactNative::ReactContext context,
        bool wantBase64,
        std::wstring facing,
        winrt::Microsoft::ReactNative::ReactPromise<JSValue> promise) noexcept
    {
        MediaCapture capture{nullptr};
        try
        {
            // 1) Hop onto the UI thread before creating/initializing MediaCapture.
            co_await ResumeOnDispatcher{context.Handle().UIDispatcher()};

            // 2) Resolve the front/back device (best effort; empty => default camera).
            const winrt::hstring deviceId = co_await PickDeviceIdAsync(facing);

            // 3) Initialize a photo-capable MediaCapture. Video-only streaming mode
            //    so we never require the microphone capability. InitializeAsync is
            //    also the consent gate on packaged apps (the `webcam`
            //    DeviceCapability prompt).
            capture = MediaCapture{};
            MediaCaptureInitializationSettings settings;
            settings.StreamingCaptureMode(StreamingCaptureMode::Video);
            settings.PhotoCaptureSource(PhotoCaptureSource::Auto);
            if (!deviceId.empty())
            {
                settings.VideoDeviceId(deviceId);
            }
            co_await capture.InitializeAsync(settings);

            // Reaching here means capture is permitted.
            g_lastPermStatus.store(1);

            // 4) Capture one full-res JPEG into LocalFolder\RNWCameraCaptures\<guid>.jpg.
            auto localFolder = ApplicationData::Current().LocalFolder();
            auto destFolder = co_await localFolder.CreateFolderAsync(
                L"RNWCameraCaptures", CreationCollisionOption::OpenIfExists);
            StorageFile file = co_await destFolder.CreateFileAsync(
                NewGuidJpgName(), CreationCollisionOption::GenerateUniqueName);

            co_await capture.CapturePhotoToStorageFileAsync(
                ImageEncodingProperties::CreateJpeg(), file);

            // 5) Release the device promptly (frees the webcam for the next shot).
            try { capture.Close(); } catch (...) {}
            capture = nullptr;

            // 6) Read back real pixel dimensions (best effort; 0 on failure).
            uint32_t width = 0, height = 0;
            try
            {
                auto stream = co_await file.OpenAsync(FileAccessMode::Read);
                auto decoder = co_await BitmapDecoder::CreateAsync(stream);
                width = decoder.PixelWidth();
                height = decoder.PixelHeight();
            }
            catch (...)
            {
                // leave 0/0
            }

            // 7) base64, when requested (upload/validation flows often read it).
            std::string base64;
            bool haveBase64 = false;
            if (wantBase64)
            {
                try
                {
                    auto buffer = co_await FileIO::ReadBufferAsync(file);
                    base64 = winrt::to_string(CryptographicBuffer::EncodeToBase64String(buffer));
                    haveBase64 = true;
                }
                catch (...)
                {
                    // omit base64 on failure
                }
            }

            // 8) Build the expo-camera CameraCapturedPicture shape.
            JSValueObject result;
            result["uri"] = MakeFileUri(file.Path());
            result["width"] = static_cast<double>(width);
            result["height"] = static_cast<double>(height);
            result["base64"] = haveBase64 ? JSValue{std::move(base64)} : JSValue{nullptr};
            // exif is intentionally null: webcam JPEGs are already upright, and
            // consumers that key off exif.Orientation would otherwise route the
            // image through a rotation pass it does not need (null => Orientation
            // 1 => no rotation).
            result["exif"] = nullptr;
            promise.Resolve(JSValue{std::move(result)});
        }
        catch (winrt::hresult_error const &e)
        {
            if (capture) { try { capture.Close(); } catch (...) {} }
            if (static_cast<int32_t>(e.code()) == static_cast<int32_t>(E_ACCESSDENIED))
            {
                g_lastPermStatus.store(2);
            }
            promise.Reject(winrt::to_string(e.message()).c_str());
        }
        catch (...)
        {
            if (capture) { try { capture.Close(); } catch (...) {} }
            promise.Reject("RNWCameraCapture: unexpected error while capturing photo");
        }
    }

    // The permission-probe coroutine. Initializes (and immediately closes) a
    // video-only MediaCapture purely to surface consent and learn granted/denied.
    winrt::fire_and_forget ProbePermissionAsync(
        winrt::Microsoft::ReactNative::ReactContext context,
        winrt::Microsoft::ReactNative::ReactPromise<JSValue> promise) noexcept
    {
        MediaCapture capture{nullptr};
        try
        {
            co_await ResumeOnDispatcher{context.Handle().UIDispatcher()};

            capture = MediaCapture{};
            MediaCaptureInitializationSettings settings;
            settings.StreamingCaptureMode(StreamingCaptureMode::Video);
            co_await capture.InitializeAsync(settings);
            try { capture.Close(); } catch (...) {}
            capture = nullptr;

            g_lastPermStatus.store(1);
            promise.Resolve(JSValue{MakePermissionObject(1, /*canAskAgain*/ true)});
        }
        catch (winrt::hresult_error const &e)
        {
            if (capture) { try { capture.Close(); } catch (...) {} }
            const bool hardDenied =
                (static_cast<int32_t>(e.code()) == static_cast<int32_t>(E_ACCESSDENIED));
            g_lastPermStatus.store(2);
            promise.Resolve(JSValue{MakePermissionObject(2, /*canAskAgain*/ !hardDenied)});
        }
        catch (...)
        {
            if (capture) { try { capture.Close(); } catch (...) {} }
            g_lastPermStatus.store(2);
            promise.Resolve(JSValue{MakePermissionObject(2, /*canAskAgain*/ true)});
        }
    }
}

namespace winrt::ReactNativeWindowsCamera::implementation
{
    void CameraCaptureModule::takePictureAsync(JSValueObject options, ReactPromise<JSValue> promise) noexcept
    {
        bool wantBase64 = false;
        if (auto it = options.find("base64"); it != options.end())
        {
            if (auto const *b = it->second.TryGetBoolean())
            {
                wantBase64 = *b;
            }
        }

        std::wstring facing = L"back";
        if (auto it = options.find("facing"); it != options.end())
        {
            if (auto const *s = it->second.TryGetString())
            {
                facing = winrt::to_hstring(*s).c_str();
            }
        }

        // Kick off the coroutine; the promise is resolved/rejected inside it.
        CaptureAsync(m_context, wantBase64, std::move(facing), promise);
    }

    void CameraCaptureModule::requestCameraPermissions(ReactPromise<JSValue> promise) noexcept
    {
        ProbePermissionAsync(m_context, promise);
    }

    JSValue CameraCaptureModule::getCameraPermissions() noexcept
    {
        return JSValue{MakePermissionObject(g_lastPermStatus.load(), /*canAskAgain*/ true)};
    }
}
