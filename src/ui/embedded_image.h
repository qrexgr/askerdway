#pragma once
#include <Windows.h>
#include <wincodec.h>
#include <cstdint>
#include <vector>

namespace embedded_image {
struct rgba_image {
    std::vector<std::uint8_t> pixels;
    int width = 0, height = 0;
};

inline bool load(HMODULE module, int id, rgba_image& output) {
    output = {};
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) return false;
    const auto loaded = LoadResource(module, resource);
    auto* bytes = static_cast<BYTE*>(LockResource(loaded));
    const auto length = SizeofResource(module, resource);
    if (!bytes || !length) return false;
    const auto com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return false;
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromMemory(bytes, length);
    if (SUCCEEDED(hr)) hr = factory->CreateDecoderFromStream(stream, nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT width = 0, height = 0;
    if (SUCCEEDED(hr)) hr = converter->GetSize(&width, &height);
    if (SUCCEEDED(hr) && width > 0 && height > 0 && width <= 4096 && height <= 4096) {
        output.pixels.resize(static_cast<std::size_t>(width) * height * 4);
        hr = converter->CopyPixels(nullptr, width * 4,
            static_cast<UINT>(output.pixels.size()), output.pixels.data());
    } else if (SUCCEEDED(hr)) hr = E_FAIL;
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    if (FAILED(hr)) { output = {}; return false; }
    output.width = static_cast<int>(width);
    output.height = static_cast<int>(height);
    return true;
}
}
