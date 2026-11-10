#include "viewer/windows_viewer_stream.hpp"

#include "capture/dxgi_output.hpp"
#include "decode/mf_h264_decoder.hpp"
#include "engine_tunables.hpp"
#include "media/media_thread_scope.hpp"

#include <windows.h>

#include <d2d1_1.h>
#include <d3d11_4.h>
#include <dwrite.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace osplay::engine {
namespace {

using Microsoft::WRL::ComPtr;

struct QueuedUnit {
    std::vector<std::uint8_t> annexb;
    bool keyframe = false;
    std::uint64_t capture_time_us = 0;
};

/// The largest rectangle with the stream's aspect ratio centred in the monitor.
RECT aspect_fit(UINT source_width, UINT source_height, UINT target_width, UINT target_height) {
    if (source_width == 0 || source_height == 0) {
        return RECT{.left = 0,
                    .top = 0,
                    .right = static_cast<LONG>(target_width),
                    .bottom = static_cast<LONG>(target_height)};
    }
    const auto scaled_width = static_cast<UINT>(static_cast<unsigned long long>(target_height) *
                                                source_width / source_height);
    if (scaled_width <= target_width) {
        const auto left = static_cast<LONG>((target_width - scaled_width) / 2);
        return RECT{.left = left,
                    .top = 0,
                    .right = left + static_cast<LONG>(scaled_width),
                    .bottom = static_cast<LONG>(target_height)};
    }
    const auto scaled_height = static_cast<UINT>(static_cast<unsigned long long>(target_width) *
                                                 source_height / source_width);
    const auto top = static_cast<LONG>((target_height - scaled_height) / 2);
    return RECT{.left = 0,
                .top = top,
                .right = static_cast<LONG>(target_width),
                .bottom = top + static_cast<LONG>(scaled_height)};
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

class WindowsViewerStream final : public ViewerStream {
public:
    WindowsViewerStream(PresentationThread& presentation, const osplay_stream_config& stream,
                        const ViewerMonitor& monitor, ViewerStreamSink& sink, bool windowed)
        : presentation_(&presentation), sink_(&sink), stream_(stream), monitor_(monitor),
          windowed_(windowed) {}

    ~WindowsViewerStream() override {
        try {
            {
                const std::scoped_lock lock(mutex_);
                stopping_ = true;
            }
            changed_.notify_all();
            if (thread_.joinable()) {
                thread_.join();
            }
            if (window_ != nullptr) {
                presentation_->destroy_window(window_);
            }
        } catch (...) {
            std::terminate(); // the decode thread cannot be left running behind its owner
        }
    }
    WindowsViewerStream(const WindowsViewerStream&) = delete;
    WindowsViewerStream& operator=(const WindowsViewerStream&) = delete;
    WindowsViewerStream(WindowsViewerStream&&) = delete;
    WindowsViewerStream& operator=(WindowsViewerStream&&) = delete;

    /// Creates the window and starts the decode thread; waits until the decoder is ready.
    std::optional<PipelineError> start(bool visible) {
        // (fork) Windowed mode: a normal fixed-size window with a caption, centered on the
        // monitor the stream would have covered — the streamed screen sits on the desktop and
        // moves with it instead of covering a display.
        window_ = windowed_ ? presentation_->create_windowed_window(
                                  monitor_.rect,
                                  SIZE{.cx = static_cast<LONG>(stream_.width),
                                       .cy = static_cast<LONG>(stream_.height)},
                                  visible)
                            : presentation_->create_window(monitor_.rect, visible);
        if (window_ == nullptr) {
            return PipelineError::PresentationFailed;
        }
        std::promise<std::optional<PipelineError>> ready;
        auto started = ready.get_future();
        thread_ =
            std::thread([this, promise = std::move(ready)]() mutable { run(std::move(promise)); });
        return started.get();
    }

    void submit(std::span<const std::uint8_t> annexb, bool keyframe,
                std::uint64_t capture_time_us) override {
        {
            const std::scoped_lock lock(mutex_);
            if (queue_.size() >= kViewerQueueLimit) {
                // The decoder fell far behind: start over from the next keyframe.
                queue_.clear();
                waiting_for_keyframe_ = true;
            }
            queue_.push_back({.annexb = std::vector<std::uint8_t>(annexb.begin(), annexb.end()),
                              .keyframe = keyframe,
                              .capture_time_us = capture_time_us});
        }
        changed_.notify_all();
    }

    void set_visible(bool visible) override { PresentationThread::show_window(window_, visible); }

    void set_overlay(const std::string& text) override {
        std::wstring wide = widen(text);
        {
            const std::scoped_lock lock(mutex_);
            if (wide == overlay_) {
                return;
            }
            overlay_ = std::move(wide);
            overlay_changed_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] ViewerStreamStats stats() const override {
        return {.frames_presented = presented_.load(), .decode_errors = errors_.load()};
    }

private:
    void run(std::promise<std::optional<PipelineError>> ready) {
        const MediaThreadScope scope;
        if (!scope.ready()) {
            ready.set_value(PipelineError::DecoderUnavailable);
            return;
        }
        if (const auto failure = initialize()) {
            release();
            ready.set_value(failure);
            return;
        }
        ready.set_value(std::nullopt);
        loop();
        release();
    }

    /// Media Foundation and COM objects go before this thread leaves its apartment.
    void release() {
        decoder_.reset();
        release_overlay();
        processor_.Reset();
        enumerator_.Reset();
        swap_chain_.Reset();
        video_context_.Reset();
        video_device_.Reset();
        context_.Reset();
        device_.Reset();
        if (waitable_ != nullptr) {
            CloseHandle(waitable_);
            waitable_ = nullptr;
        }
    }

    std::optional<PipelineError> initialize() {
        const auto adapter = adapter_for_monitor(monitor_.rect);
        if (FAILED(D3D11CreateDevice(
                adapter.Get(),
                adapter == nullptr ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
            return PipelineError::DecoderUnavailable;
        }
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(device_.As(&multithread))) {
            multithread->SetMultithreadProtected(TRUE); // the decoder uses the device too
        }
        if (FAILED(device_.As(&video_device_)) || FAILED(context_.As(&video_context_))) {
            return PipelineError::DecoderUnavailable;
        }
        if (!create_swap_chain()) {
            return PipelineError::PresentationFailed;
        }
        auto decoder = MfH264Decoder::create(
            device_.Get(), stream_.width, stream_.height,
            [this](ID3D11Texture2D* texture, UINT slice, std::uint64_t /*capture_time_us*/) {
                present(texture, slice);
            });
        if (!decoder) {
            return decoder.error();
        }
        decoder_ = std::move(decoder.value());
        return std::nullopt;
    }

    bool create_swap_chain() {
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device_.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = monitor_.description.width_px;
        desc.Height = monitor_.description.height_px;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_NONE; // the window has exactly the monitor's size
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        ComPtr<IDXGISwapChain1> swap_chain;
        if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), window_, &desc, nullptr, nullptr,
                                                   &swap_chain)) ||
            FAILED(swap_chain.As(&swap_chain_)) || FAILED(swap_chain_->SetMaximumFrameLatency(1))) {
            return false;
        }
        static_cast<void>(factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER));
        waitable_ = swap_chain_->GetFrameLatencyWaitableObject();
        return true;
    }

    void loop() {
        while (true) {
            QueuedUnit unit;
            bool skip = false;
            std::optional<std::wstring> overlay;
            {
                std::unique_lock lock(mutex_);
                changed_.wait(lock,
                              [this] { return stopping_ || !queue_.empty() || overlay_changed_; });
                if (stopping_) {
                    return;
                }
                if (overlay_changed_) {
                    overlay = overlay_;
                    overlay_changed_ = false;
                }
            }
            if (overlay) {
                update_overlay(std::move(*overlay));
            }
            {
                const std::scoped_lock lock(mutex_);
                if (queue_.empty()) {
                    continue;
                }
                unit = std::move(queue_.front());
                queue_.pop_front();
                if (waiting_for_keyframe_ && !unit.keyframe) {
                    skip = true; // a frame that depends on lost data would only show garbage
                } else {
                    waiting_for_keyframe_ = false;
                }
            }
            if (skip) {
                notify_keyframe_needed();
                continue;
            }
            if (!decoder_->decode(unit.annexb, unit.capture_time_us)) {
                ++errors_;
                {
                    const std::scoped_lock lock(mutex_);
                    waiting_for_keyframe_ = true;
                }
                notify_keyframe_needed();
            }
        }
    }

    void notify_keyframe_needed() {
        const ULONGLONG now = GetTickCount64();
        if (now - last_keyframe_request_ >= kViewerKeyframeRequestSpacingMilliseconds) {
            last_keyframe_request_ = now;
            sink_->on_keyframe_needed(stream_.stream_id);
        }
    }

    /// Converts the decoded slice into the back buffer and presents it at once.
    void present(ID3D11Texture2D* texture, UINT slice) {
        wait_for_swap_chain();
        if (!overlay_text_.empty()) {
            // The overlay needs the picture kept apart, so a new text can be drawn without a frame.
            if (!create_frame() || !blit(texture, slice, frame_.Get())) {
                return;
            }
            frame_valid_ = true;
            if (present_frame()) {
                ++presented_;
            }
            return;
        }
        ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer))) ||
            !blit(texture, slice, back_buffer.Get())) {
            return;
        }
        if (SUCCEEDED(swap_chain_->Present(0, 0))) {
            ++presented_;
        }
    }

    /// At most one frame queued for the display: wait until the previous one is picked up.
    void wait_for_swap_chain() const {
        if (waitable_ != nullptr) {
            WaitForSingleObjectEx(waitable_, kPresentWaitMilliseconds, TRUE);
        }
    }

    /// A new overlay text: shown on the last picture at once, even while the screen is still.
    void update_overlay(std::wstring text) {
        overlay_text_ = std::move(text);
        if (frame_valid_) {
            wait_for_swap_chain();
            static_cast<void>(present_frame()); // a failed redraw waits for the next frame
        }
        if (overlay_text_.empty()) {
            frame_.Reset(); // back to decoding straight into the swap chain
            frame_valid_ = false;
        }
    }

    bool create_frame() {
        if (frame_ != nullptr) {
            return true;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = monitor_.description.width_px;
        desc.Height = monitor_.description.height_px;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        return SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &frame_));
    }

    /// The kept picture with the overlay over it.
    bool present_frame() {
        ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
            return false;
        }
        context_->CopyResource(back_buffer.Get(), frame_.Get());
        draw_overlay(back_buffer.Get()); // the picture still goes out if text cannot be drawn
        return SUCCEEDED(swap_chain_->Present(0, 0));
    }

    void draw_overlay(ID3D11Texture2D* back_buffer) {
        if (overlay_text_.empty() || !create_overlay_resources(back_buffer)) {
            return;
        }
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(write_factory_->CreateTextLayout(
                overlay_text_.c_str(), static_cast<UINT32>(overlay_text_.size()),
                text_format_.Get(), static_cast<float>(monitor_.description.width_px),
                static_cast<float>(monitor_.description.height_px), &layout))) {
            return;
        }
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics))) {
            return;
        }
        const float scale = overlay_scale();
        const float margin = kOverlayMargin * scale;
        const float padding = kOverlayPadding * scale;
        const float radius = kOverlayCornerRadius * scale;
        const D2D1_RECT_F box{.left = margin,
                              .top = margin,
                              .right = margin + metrics.width + (2 * padding),
                              .bottom = margin + metrics.height + (2 * padding)};
        draw_context_->SetTarget(draw_target_.Get());
        draw_context_->BeginDraw();
        draw_context_->FillRoundedRectangle(
            D2D1_ROUNDED_RECT{.rect = box, .radiusX = radius, .radiusY = radius},
            background_brush_.Get());
        draw_context_->DrawTextLayout(D2D1_POINT_2F{.x = margin + padding, .y = margin + padding},
                                      layout.Get(), text_brush_.Get());
        static_cast<void>(draw_context_->EndDraw()); // a lost frame of text is redrawn next time
        draw_context_->SetTarget(nullptr);
    }

    /// The monitor's scaling, so the overlay has the same size in points as on the Mac.
    [[nodiscard]] float overlay_scale() const {
        const auto percent = monitor_.description.scale_percent;
        return percent == 0 ? 1.0F : static_cast<float>(percent) / 100.0F;
    }

    /// Direct2D and DirectWrite objects, made on first use of the overlay.
    bool create_overlay_resources(ID3D11Texture2D* back_buffer) {
        if (draw_target_ != nullptr) {
            return true;
        }
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<ID2D1Device> draw_device;
        ComPtr<IDXGISurface> surface;
        // Multithreaded: the decoder uses the same D3D11 device from Media Foundation's threads.
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                                     draw_factory_.GetAddressOf())) ||
            FAILED(device_.As(&dxgi_device)) ||
            FAILED(draw_factory_->CreateDevice(dxgi_device.Get(), &draw_device)) ||
            FAILED(draw_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                                    &draw_context_)) ||
            FAILED(
                DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                                    reinterpret_cast<IUnknown**>(write_factory_.GetAddressOf()))) ||
            FAILED(write_factory_->CreateTextFormat(
                kOverlayFontFamily, nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, kOverlayFontSize * overlay_scale(), L"",
                &text_format_)) ||
            FAILED(draw_context_->CreateSolidColorBrush(
                D2D1_COLOR_F{.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = kOverlayBackgroundOpacity},
                &background_brush_)) ||
            FAILED(draw_context_->CreateSolidColorBrush(color(kOverlayTextColor), &text_brush_)) ||
            FAILED(back_buffer->QueryInterface(IID_PPV_ARGS(&surface)))) {
            release_overlay();
            return false;
        }
        text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        // The flip-model back buffer 0 always names the buffer being drawn, so one target serves
        // every frame.
        const D2D1_BITMAP_PROPERTIES1 properties{
            .pixelFormat = {.format = DXGI_FORMAT_B8G8R8A8_UNORM,
                            .alphaMode = D2D1_ALPHA_MODE_IGNORE},
            .dpiX = 96.0F,
            .dpiY = 96.0F,
            .bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            .colorContext = nullptr};
        if (FAILED(draw_context_->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                              &draw_target_))) {
            release_overlay();
            return false;
        }
        return true;
    }

    static D2D1_COLOR_F color(std::uint32_t rgb) {
        constexpr float kMax = 255.0F;
        return {.r = static_cast<float>((rgb >> 16U) & 0xFFU) / kMax,
                .g = static_cast<float>((rgb >> 8U) & 0xFFU) / kMax,
                .b = static_cast<float>(rgb & 0xFFU) / kMax,
                .a = 1.0F};
    }

    void release_overlay() {
        draw_target_.Reset();
        text_brush_.Reset();
        background_brush_.Reset();
        text_format_.Reset();
        write_factory_.Reset();
        draw_context_.Reset();
        draw_factory_.Reset();
        frame_.Reset();
        frame_valid_ = false;
    }

    bool blit(ID3D11Texture2D* source, UINT slice, ID3D11Texture2D* target) {
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        const UINT target_width = monitor_.description.width_px;
        const UINT target_height = monitor_.description.height_px;
        if (processor_ == nullptr || source_desc.Width != processor_width_ ||
            source_desc.Height != processor_height_) {
            if (!create_processor(source_desc.Width, source_desc.Height, target_width,
                                  target_height)) {
                return false;
            }
        }
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access): D3D11 API
        input_desc.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> input;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
        output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> output;
        if (FAILED(video_device_->CreateVideoProcessorInputView(source, enumerator_.Get(),
                                                                &input_desc, &input)) ||
            FAILED(video_device_->CreateVideoProcessorOutputView(target, enumerator_.Get(),
                                                                 &output_desc, &output))) {
            return false;
        }
        // The decoder pads its textures; only the stream's area is picture.
        const RECT source_rect{.left = 0,
                               .top = 0,
                               .right = static_cast<LONG>(stream_.width),
                               .bottom = static_cast<LONG>(stream_.height)};
        const RECT destination =
            aspect_fit(stream_.width, stream_.height, target_width, target_height);
        video_context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source_rect);
        video_context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &destination);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = input.Get();
        return SUCCEEDED(
            video_context_->VideoProcessorBlt(processor_.Get(), output.Get(), 0, 1, &stream));
    }

    bool create_processor(UINT source_width, UINT source_height, UINT target_width,
                          UINT target_height) {
        processor_.Reset();
        enumerator_.Reset();
        const DXGI_RATIONAL rate{.Numerator = stream_.refresh_millihertz, .Denominator = 1000};
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = rate;
        desc.InputWidth = source_width;
        desc.InputHeight = source_height;
        desc.OutputFrameRate = rate;
        desc.OutputWidth = target_width;
        desc.OutputHeight = target_height;
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        if (FAILED(video_device_->CreateVideoProcessorEnumerator(&desc, &enumerator_)) ||
            FAILED(video_device_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_))) {
            return false;
        }
        video_context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
        video_context_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0,
                                                           D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        D3D11_VIDEO_COLOR black{};
        black.RGBA.A = 1.0F; // NOLINT(cppcoreguidelines-pro-type-union-access): D3D11 API
        video_context_->VideoProcessorSetOutputBackgroundColor(processor_.Get(), FALSE, &black);
        ComPtr<ID3D11VideoContext1> context1;
        if (SUCCEEDED(video_context_.As(&context1))) {
            context1->VideoProcessorSetStreamColorSpace1(
                processor_.Get(), 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            context1->VideoProcessorSetOutputColorSpace1(processor_.Get(),
                                                         DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }
        processor_width_ = source_width;
        processor_height_ = source_height;
        return true;
    }

    PresentationThread* presentation_; // never null
    ViewerStreamSink* sink_;           // never null
    osplay_stream_config stream_;
    ViewerMonitor monitor_;
    bool windowed_ = false; ///< (fork) present in a window instead of covering a monitor
    HWND window_ = nullptr;

    // Decode thread only.
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11VideoDevice> video_device_;
    ComPtr<ID3D11VideoContext> video_context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    UINT processor_width_ = 0;
    UINT processor_height_ = 0;
    ComPtr<IDXGISwapChain2> swap_chain_;
    HANDLE waitable_ = nullptr;
    std::unique_ptr<MfH264Decoder> decoder_;
    ULONGLONG last_keyframe_request_ = 0;
    std::wstring overlay_text_;     // what the overlay shows; empty while hidden
    ComPtr<ID3D11Texture2D> frame_; // the picture, kept only while the overlay shows
    bool frame_valid_ = false;
    ComPtr<ID2D1Factory1> draw_factory_;
    ComPtr<ID2D1DeviceContext> draw_context_;
    ComPtr<ID2D1Bitmap1> draw_target_;
    ComPtr<IDWriteFactory> write_factory_;
    ComPtr<IDWriteTextFormat> text_format_;
    ComPtr<ID2D1SolidColorBrush> background_brush_;
    ComPtr<ID2D1SolidColorBrush> text_brush_;

    std::atomic<std::uint64_t> presented_{0};
    std::atomic<std::uint64_t> errors_{0};

    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<QueuedUnit> queue_;
    bool waiting_for_keyframe_ = true;
    bool stopping_ = false;
    std::wstring overlay_;         // set by the shell's thread
    bool overlay_changed_ = false; // tells the decode thread to redraw
    std::thread thread_;
};

} // namespace

win::Expected<std::unique_ptr<ViewerStream>, PipelineError>
WindowsViewerStreamFactory::start(const osplay_stream_config& stream, const ViewerMonitor& monitor,
                                  bool visible, ViewerStreamSink& sink) {
    using Result = win::Expected<std::unique_ptr<ViewerStream>, PipelineError>;
    auto viewer_stream =
        std::make_unique<WindowsViewerStream>(*presentation_, stream, monitor, sink, windowed_);
    if (const auto failure = viewer_stream->start(visible)) {
        return Result::failure(*failure);
    }
    return {std::unique_ptr<ViewerStream>(std::move(viewer_stream))};
}

} // namespace osplay::engine
