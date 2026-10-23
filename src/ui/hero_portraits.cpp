/**
 * @file
 * @brief Загрузка и композиция портретов.
 *
 * Декодирует встроенный атлас через WIC и рисует круглый портрет в BGRA DIB. Имя героя
 * сопоставляется с manifest; исходные пиксели сохраняются для повторного применения tint без
 * накопления искажений.
 */

#include "hero_portraits.h"

#include "hero_portrait_manifest.h"
#include "sidebar_icon_resources.h"

#include <Windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace hero_portraits {
namespace {

std::vector<std::uint8_t> g_rgba_pixels;
int g_atlas_width = 0;
int g_atlas_height = 0;

const hero_portrait_manifest::entry* find_entry(const char* designer_name) {
    if (designer_name == nullptr || *designer_name == '\0')
        return nullptr;
    for (const auto& entry : hero_portrait_manifest::entries) {
        if (std::strcmp(entry.designer_name, designer_name) == 0)
            return &entry;
    }
    return nullptr;
}

std::uint8_t channel(float value) {
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(std::lround(value)), 0, 255));
}

} // namespace

bool initialize(void* module_handle) {
    shutdown();
    if (module_handle == nullptr)
        return false;

    const HMODULE module = static_cast<HMODULE>(module_handle);
    const HRSRC resource = FindResourceW(module,
        MAKEINTRESOURCEW(IDR_HERO_PORTRAIT_ATLAS), RT_RCDATA);
    if (resource == nullptr)
        return false;
    const HGLOBAL loaded_resource = LoadResource(module, resource);
    const DWORD encoded_size = SizeofResource(module, resource);
    auto* encoded_data = static_cast<BYTE*>(LockResource(loaded_resource));
    if (loaded_resource == nullptr || encoded_data == nullptr || encoded_size == 0)
        return false;

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE)
        return false;

    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
        result = factory->CreateStream(&stream);
    if (SUCCEEDED(result))
        result = stream->InitializeFromMemory(encoded_data, encoded_size);
    if (SUCCEEDED(result))
        result = factory->CreateDecoderFromStream(stream, nullptr,
            WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result))
        result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result))
        result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result))
        result = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom);

    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(result))
        result = converter->GetSize(&width, &height);
    if (SUCCEEDED(result) && width > 0 && height > 0 &&
        width <= 4096 && height <= 4096) {
        g_rgba_pixels.resize(static_cast<std::size_t>(width) * height * 4);
        result = converter->CopyPixels(nullptr, width * 4,
            static_cast<UINT>(g_rgba_pixels.size()), g_rgba_pixels.data());
    } else if (SUCCEEDED(result)) {
        result = E_FAIL;
    }

    if (converter != nullptr)
        converter->Release();
    if (frame != nullptr)
        frame->Release();
    if (decoder != nullptr)
        decoder->Release();
    if (stream != nullptr)
        stream->Release();
    if (factory != nullptr)
        factory->Release();
    if (uninitialize_com)
        CoUninitialize();

    if (FAILED(result)) {
        shutdown();
        return false;
    }
    g_atlas_width = static_cast<int>(width);
    g_atlas_height = static_cast<int>(height);
    return true;
}

void shutdown() {
    g_rgba_pixels.clear();
    g_rgba_pixels.shrink_to_fit();
    g_atlas_width = 0;
    g_atlas_height = 0;
}

bool is_available(const char* designer_name) {
    return !g_rgba_pixels.empty() && find_entry(designer_name) != nullptr;
}

bool blit_bgra(
    std::uint8_t* destination,
    int destination_width,
    int destination_height,
    int destination_stride,
    int center_x,
    int center_y,
    int diameter,
    const char* designer_name,
    bool blue_tint,
    float opacity) {
    const auto* entry = find_entry(designer_name);
    if (destination == nullptr || destination_width <= 0 ||
        destination_height <= 0 || destination_stride < destination_width * 4 ||
        diameter <= 0 || entry == nullptr || g_rgba_pixels.empty())
        return false;

    opacity = std::clamp(opacity, 0.0f, 1.0f);
    const int tile = hero_portrait_manifest::tile_size;
    const int source_x = static_cast<int>(entry->column) * tile;
    const int source_y = static_cast<int>(entry->row) * tile;
    if (source_x < 0 || source_y < 0 || source_x + tile > g_atlas_width ||
        source_y + tile > g_atlas_height)
        return false;

    const int left = center_x - diameter / 2;
    const int top = center_y - diameter / 2;
    const float radius = static_cast<float>(diameter) * 0.5f;
    const float radius_squared = radius * radius;
    for (int y = 0; y < diameter; ++y) {
        const int destination_y = top + y;
        if (destination_y < 0 || destination_y >= destination_height)
            continue;
        for (int x = 0; x < diameter; ++x) {
            const int destination_x = left + x;
            if (destination_x < 0 || destination_x >= destination_width)
                continue;
            const float circle_x = static_cast<float>(x) + 0.5f - radius;
            const float circle_y = static_cast<float>(y) + 0.5f - radius;
            if (circle_x * circle_x + circle_y * circle_y > radius_squared)
                continue;

            const int sample_x = source_x + std::clamp(
                static_cast<int>((static_cast<float>(x) + 0.5f) * tile /
                    static_cast<float>(diameter)), 0, tile - 1);
            const int sample_y = source_y + std::clamp(
                static_cast<int>((static_cast<float>(y) + 0.5f) * tile /
                    static_cast<float>(diameter)), 0, tile - 1);
            const std::size_t source_offset =
                (static_cast<std::size_t>(sample_y) * g_atlas_width + sample_x) * 4;
            float red = static_cast<float>(g_rgba_pixels[source_offset + 0]);
            float green = static_cast<float>(g_rgba_pixels[source_offset + 1]);
            float blue = static_cast<float>(g_rgba_pixels[source_offset + 2]);
            const float alpha =
                static_cast<float>(g_rgba_pixels[source_offset + 3]) / 255.0f * opacity;
            if (alpha <= 0.001f)
                continue;

            if (blue_tint) {
                // Apply the tint to the source matrix at draw time. Luminance
                // preserves facial detail while suppressing red/green channels.
                const float luminance = red * 0.2126f + green * 0.7152f + blue * 0.0722f;
                red = luminance * 0.16f;
                green = luminance * 0.48f;
                blue = std::min(255.0f, 38.0f + luminance * 1.30f);
            }

            std::uint8_t* output = destination +
                static_cast<std::size_t>(destination_y) * destination_stride +
                static_cast<std::size_t>(destination_x) * 4;
            output[0] = channel(blue * alpha + output[0] * (1.0f - alpha));
            output[1] = channel(green * alpha + output[1] * (1.0f - alpha));
            output[2] = channel(red * alpha + output[2] * (1.0f - alpha));
            // The minimap overlay uses exact black as its transparency key.
            // Preserve genuinely dark portrait pixels by nudging only a fully
            // black composed pixel outside that reserved color.
            if (output[0] == 0 && output[1] == 0 && output[2] == 0)
                output[0] = 1;
            output[3] = 255;
        }
    }
    return true;
}

} // namespace hero_portraits
