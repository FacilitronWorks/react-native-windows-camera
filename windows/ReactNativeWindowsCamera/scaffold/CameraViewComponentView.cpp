// ⚠️ SCAFFOLD — EXCLUDED FROM BUILD (see CameraViewComponentView.h header).
#include "pch.h"
#include "CameraViewComponentView.h"

// RN Fabric / Composition component-view surface (ContentIslandComponentView,
// IReactCompositionViewComponentBuilder).
#include <winrt/Microsoft.ReactNative.Composition.h>

// Capture stack — pure Windows SDK projections; NO extra NuGet needed. Same
// set as CameraCaptureModule.
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

using namespace winrt;
using namespace winrt::Microsoft::ReactNative;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Security::Cryptography;
// NOTE: do not alias `comp` — RNW's CppWinRTIncludes.h already defines
// `comp = winrt::Microsoft::UI::Composition` at global scope (C2881 otherwise).
namespace rncomp = winrt::Microsoft::ReactNative::Composition;

namespace
{
    // --- shared small helpers (kept local so this file stays self-contained) ---

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
                uri.push_back('/');
            else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                     (ch >= '0' && ch <= '9') || ch == '/' || ch == ':' ||
                     ch == '.' || ch == '-' || ch == '_' || ch == '~')
                uri.push_back(static_cast<char>(ch));
            else
            {
                uri.push_back('%');
                uri.push_back(kHex[ch >> 4]);
                uri.push_back(kHex[ch & 0x0F]);
            }
        }
        return uri;
    }

    winrt::hstring NewGuidJpgName()
    {
        GUID g{};
        (void)::CoCreateGuid(&g);
        wchar_t buf[64] = {};
        int n = ::StringFromGUID2(g, buf, static_cast<int>(std::size(buf)));
        std::wstring s = (n > 2) ? std::wstring(buf + 1, buf + (n - 2)) : std::wstring(buf);
        s.append(L".jpg");
        return winrt::hstring{s};
    }

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
                    fallback = d.Id();
                auto loc = d.EnclosureLocation();
                if (loc && loc.Panel() == wanted)
                    co_return d.Id();
            }
        }
        catch (...)
        {
        }
        co_return fallback;
    }

    // Fully consume the value the reader is on (keeps the reader in sync for
    // unknown props).
    void SkipValue(IJSValueReader const &r) noexcept
    {
        try
        {
            switch (r.ValueType())
            {
            case JSValueType::String: r.GetString(); break;
            case JSValueType::Boolean: r.GetBoolean(); break;
            case JSValueType::Int64: r.GetInt64(); break;
            case JSValueType::Double: r.GetDouble(); break;
            case JSValueType::Object: { winrt::hstring k; while (r.GetNextObjectProperty(k)) SkipValue(r); break; }
            case JSValueType::Array: while (r.GetNextArrayItem()) SkipValue(r); break;
            default: break;
            }
        }
        catch (...) {}
    }

    // ------------------------------------------------------------------------
    // Prop bag for <CameraView/>:
    //   facing: 'front' | 'back'      (which camera)
    //   flash:  'off' | 'on' | 'auto' (tracked; hardware flash is rare on
    //                                  webcams -> no-op today)
    // ------------------------------------------------------------------------
    struct CameraViewProps : winrt::implements<CameraViewProps, IComponentProps>
    {
        CameraViewProps(ViewProps props, IComponentProps cloneFrom) : m_viewProps(props)
        {
            if (cloneFrom)
            {
                auto prior = cloneFrom.as<CameraViewProps>();
                facing = prior->facing;
                flash = prior->flash;
            }
        }

        void SetProp(uint32_t /*hash*/, winrt::hstring propName, IJSValueReader value) noexcept
        {
            const std::wstring name{propName};
            if (name == L"facing")
                facing = value.ValueType() == JSValueType::String ? value.GetString() : winrt::hstring{L"back"};
            else if (name == L"flash")
                flash = value.ValueType() == JSValueType::String ? value.GetString() : winrt::hstring{L"off"};
            else
                SkipValue(value);
        }

        ViewProps m_viewProps{nullptr};
        winrt::hstring facing{L"back"};
        winrt::hstring flash{L"off"};
    };

    // ------------------------------------------------------------------------
    // Per-instance native state for one mounted <CameraView/>. Owns the
    // MediaCapture and the RN ContentIslandComponentView + EventEmitter.
    // ------------------------------------------------------------------------
    struct CameraViewInstance : winrt::implements<CameraViewInstance, IInspectable>
    {
        rncomp::ContentIslandComponentView componentView{nullptr};
        EventEmitter eventEmitter{nullptr};

        MediaCapture capture{nullptr};
        std::wstring facing{L"back"};
        std::atomic<bool> ready{false};
        std::atomic<bool> busy{false}; // guards concurrent init/reinit/capture

        // Bring up the MediaCapture (headless — no preview surface yet; see the
        // header). Capture works from the initialized session even without a
        // visible viewfinder, so ref.takePictureAsync() succeeds regardless.
        void Bringup() noexcept
        {
            InitAsync(facing); // fire-and-forget async init of the MediaCapture

            // --- TODO(preview surface) — the next milestone ---
            // WinUI 3 has no CaptureElement. To show a live viewfinder, drive a
            // MediaFrameReader off `capture`, blit each frame into a
            // Microsoft.UI Composition surface (CompositionDrawingSurface /
            // SpriteVisual), wrap that visual in a
            // Microsoft.UI.Content.ContentIsland, and hand it to
            // `componentView.Connect(island)` — the identical island handoff
            // our WebView2 component performs in production (see
            // FacilitronWorks/rnw-native-core for the reusable pattern).
        }

        winrt::fire_and_forget InitAsync(std::wstring wantFacing) noexcept
        {
            auto strongThis = get_strong();
            if (busy.exchange(true))
                co_return; // another init/capture in flight; skip
            try
            {
                if (capture)
                {
                    try { capture.Close(); } catch (...) {}
                    capture = nullptr;
                    ready.store(false);
                }

                const winrt::hstring deviceId = co_await PickDeviceIdAsync(wantFacing);

                MediaCapture mc;
                MediaCaptureInitializationSettings settings;
                settings.StreamingCaptureMode(StreamingCaptureMode::Video);
                settings.PhotoCaptureSource(PhotoCaptureSource::Auto);
                if (!deviceId.empty())
                    settings.VideoDeviceId(deviceId);
                co_await mc.InitializeAsync(settings);

                capture = mc;
                facing = wantFacing;
                ready.store(true);
            }
            catch (...)
            {
                OutputDebugStringA("RNWCameraView: MediaCapture init failed\n");
                ready.store(false);
            }
            busy.store(false);
        }

        // Re-init to the other camera on a facing prop change.
        void OnFacingChanged(winrt::hstring const &newFacing) noexcept
        {
            const std::wstring f{newFacing};
            if (f == facing)
                return;
            InitAsync(f);
        }

        // "takePicture" command -> capture a still from the live MediaCapture
        // and emit topPictureTaken. On any failure we still emit (with an empty
        // uri) so the JS surface can fall back to the RNWCameraCapture module
        // without waiting for a timeout.
        winrt::fire_and_forget CaptureAndEmit(winrt::hstring requestId, bool wantBase64) noexcept
        {
            auto strongThis = get_strong();
            std::string uri;
            uint32_t width = 0, height = 0;
            std::string base64;
            bool haveBase64 = false;
            try
            {
                if (!capture || !ready.load())
                    throw winrt::hresult_error(E_NOT_VALID_STATE, L"camera not ready");

                auto localFolder = ApplicationData::Current().LocalFolder();
                auto destFolder = co_await localFolder.CreateFolderAsync(
                    L"RNWCameraCaptures", CreationCollisionOption::OpenIfExists);
                StorageFile file = co_await destFolder.CreateFileAsync(
                    NewGuidJpgName(), CreationCollisionOption::GenerateUniqueName);

                co_await capture.CapturePhotoToStorageFileAsync(
                    ImageEncodingProperties::CreateJpeg(), file);

                uri = MakeFileUri(file.Path());

                try
                {
                    auto stream = co_await file.OpenAsync(FileAccessMode::Read);
                    auto decoder = co_await BitmapDecoder::CreateAsync(stream);
                    width = decoder.PixelWidth();
                    height = decoder.PixelHeight();
                }
                catch (...) {}

                if (wantBase64)
                {
                    try
                    {
                        auto buffer = co_await FileIO::ReadBufferAsync(file);
                        base64 = winrt::to_string(CryptographicBuffer::EncodeToBase64String(buffer));
                        haveBase64 = true;
                    }
                    catch (...) {}
                }
            }
            catch (...)
            {
                OutputDebugStringA("RNWCameraView: takePicture failed\n");
                uri.clear(); // signal failure to JS (empty uri => JS falls back)
            }
            EmitPictureTaken(requestId, uri, width, height, haveBase64, base64);
        }

        void EmitPictureTaken(winrt::hstring const &requestId, std::string const &uri,
                              uint32_t width, uint32_t height, bool haveBase64,
                              std::string const &base64) noexcept
        {
            if (!eventEmitter)
                return;
            eventEmitter.DispatchEvent(
                L"topPictureTaken",
                [requestId, uri, width, height, haveBase64, base64](IJSValueWriter const &writer) {
                writer.WriteObjectBegin();
                writer.WritePropertyName(L"requestId");
                writer.WriteString(requestId);
                writer.WritePropertyName(L"uri");
                writer.WriteString(winrt::to_hstring(uri));
                writer.WritePropertyName(L"width");
                writer.WriteDouble(static_cast<double>(width));
                writer.WritePropertyName(L"height");
                writer.WriteDouble(static_cast<double>(height));
                writer.WritePropertyName(L"base64");
                if (haveBase64)
                    writer.WriteString(winrt::to_hstring(base64));
                else
                    writer.WriteNull();
                writer.WritePropertyName(L"exif");
                writer.WriteNull();
                writer.WriteObjectEnd();
            });
        }

        // Unmount teardown: stop event dispatch toward a dead JS view and
        // release the webcam. Called from the ComponentView's Destroying handler
        // (UI thread). An in-flight CaptureAndEmit holds a strong ref, so the
        // instance stays valid through its co_awaits; nulling eventEmitter here
        // makes its final EmitPictureTaken a no-op instead of a stale dispatch.
        void Teardown() noexcept
        {
            ready.store(false);
            eventEmitter = nullptr;
            componentView = nullptr;
            if (capture)
            {
                try { capture.Close(); } catch (...) {}
                capture = nullptr;
            }
        }

        void HandleCommand(winrt::hstring const &commandName, IJSValueReader const &argReader) noexcept
        {
            if (std::wstring{commandName} != L"takePicture")
                return;
            // args = [requestId, wantBase64?]
            winrt::hstring requestId;
            bool wantBase64 = true;
            try
            {
                if (argReader.ValueType() == JSValueType::Array)
                {
                    if (argReader.GetNextArrayItem())
                    {
                        if (argReader.ValueType() == JSValueType::String)
                            requestId = argReader.GetString();
                        else if (argReader.ValueType() == JSValueType::Int64)
                            requestId = winrt::to_hstring(argReader.GetInt64());
                        else if (argReader.ValueType() == JSValueType::Double)
                            requestId = winrt::to_hstring(static_cast<int64_t>(argReader.GetDouble()));
                        else
                            SkipValue(argReader);
                    }
                    if (argReader.GetNextArrayItem() && argReader.ValueType() == JSValueType::Boolean)
                        wantBase64 = argReader.GetBoolean();
                }
            }
            catch (...) {}
            CaptureAndEmit(requestId, wantBase64);
        }
    };

    // ------------------------------------------------------------------------
    // Registry: mounted view -> instance, keyed by the stable Fabric Tag.
    // Entries are erased by the view's Destroying handler (see the initializer
    // below), which also tears down the instance — no growth across a session,
    // no stale EventEmitter dispatch after unmount. (THE TEARDOWN FIX: an
    // earlier iteration keyed on COM identity and never erased; do not regress
    // this.)
    // ------------------------------------------------------------------------
    std::mutex g_instancesMutex;
    std::unordered_map<int32_t, winrt::com_ptr<CameraViewInstance>> g_instances;

    winrt::com_ptr<CameraViewInstance> FindInstance(ComponentView const &view) noexcept
    {
        try
        {
            std::scoped_lock lock(g_instancesMutex);
            auto it = g_instances.find(view.Tag());
            return it == g_instances.end() ? nullptr : it->second;
        }
        catch (...)
        {
            return nullptr;
        }
    }
}

namespace winrt::ReactNativeWindowsCamera::implementation
{
    void CameraViewComponentView::RegisterViewComponent(
        winrt::Microsoft::ReactNative::IReactPackageBuilder const &packageBuilder) noexcept
    {
        auto fabricBuilder = packageBuilder.as<IReactPackageBuilderFabric>();

        fabricBuilder.AddViewComponent(
            L"RNWCameraView", [](IReactViewComponentBuilder const &builder) noexcept {
                // 1) Custom props factory (facing / flash).
                builder.SetCreateProps(
                    [](ViewProps props, IComponentProps cloneFrom) noexcept -> IComponentProps {
                        return winrt::make<CameraViewProps>(props, cloneFrom);
                    });

                // 2) Island-backed component view: stand up the MediaCapture +
                //    (TODO) preview island.
                auto compBuilder = builder.as<rncomp::IReactCompositionViewComponentBuilder>();
                compBuilder.SetContentIslandComponentViewInitializer(
                    [](rncomp::ContentIslandComponentView const &view) noexcept {
                        auto instance = winrt::make_self<CameraViewInstance>();
                        instance->componentView = view;
                        {
                            std::scoped_lock lock(g_instancesMutex);
                            g_instances[view.Tag()] = instance;
                        }
                        // Teardown hook: when RN destroys the view (unmount /
                        // instance shutdown), erase the registry entry and
                        // release the camera.
                        view.Destroying(
                            [](IInspectable const & /*sender*/, ComponentView const &v) noexcept {
                                winrt::com_ptr<CameraViewInstance> dying;
                                {
                                    std::scoped_lock lock(g_instancesMutex);
                                    auto it = g_instances.find(v.Tag());
                                    if (it != g_instances.end())
                                    {
                                        dying = std::move(it->second);
                                        g_instances.erase(it);
                                    }
                                }
                                if (dying)
                                {
                                    dying->Teardown();
                                }
                            });
                        instance->Bringup();
                    });

                // 3) Prop updates -> re-init camera on facing change.
                builder.SetUpdatePropsHandler(
                    [](ComponentView const &source, IComponentProps const &newProps, IComponentProps const & /*oldProps*/) noexcept {
                        auto instance = FindInstance(source);
                        if (!instance || !newProps)
                            return;
                        auto p = newProps.as<CameraViewProps>();
                        instance->OnFacingChanged(p->facing);
                    });

                // 4) Event emitter -> onPictureTaken sink.
                builder.SetUpdateEventEmitterHandler(
                    [](ComponentView const &source, EventEmitter const &eventEmitter) noexcept {
                        if (auto instance = FindInstance(source))
                            instance->eventEmitter = eventEmitter;
                    });

                // 5) Imperative command: takePicture.
                builder.SetCustomCommandHandler(
                    [](ComponentView const &source, HandleCommandArgs const &args) noexcept {
                        if (auto instance = FindInstance(source))
                        {
                            instance->HandleCommand(args.CommandName(), args.CommandArgs());
                            args.Handled(true);
                        }
                    });
            });
    }
}
