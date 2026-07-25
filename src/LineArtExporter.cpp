#include "LineArtExporter.hpp"

#include "LineArtRenderer.hpp"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <string>
#include <utility>

namespace vrcdraw {
namespace {

template <typename T>
class ComPtr {
public:
    ~ComPtr()
    {
        if (value_ != nullptr) {
            value_->Release();
        }
    }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr() = default;

    [[nodiscard]] T* Get() const { return value_; }
    [[nodiscard]] T** Put()
    {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
        return &value_;
    }
    T* operator->() const { return value_; }

private:
    T* value_{};
};

class ComApartment {
public:
    ComApartment()
    {
        result_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        shouldUninitialize_ = SUCCEEDED(result_);
    }
    ~ComApartment()
    {
        if (shouldUninitialize_) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool IsAvailable() const
    {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT result_{E_FAIL};
    bool shouldUninitialize_{};
};

std::wstring HResultMessage(const wchar_t* operation, const HRESULT result)
{
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(result));
    return std::wstring(operation) + L"失败（" + buffer + L"）。";
}

std::wstring UniqueBaseName(
    const std::filesystem::path& directory,
    const std::wstring& baseName)
{
    std::wstring candidate = baseName;
    for (int suffix = 1;
         std::filesystem::exists(directory / (candidate + L".png")) ||
         std::filesystem::exists(directory / (candidate + L".svg"));
         ++suffix) {
        wchar_t number[16]{};
        swprintf_s(number, L"-%03d", suffix);
        candidate = baseName + number;
    }
    return candidate;
}

std::expected<void, std::wstring> WritePng(
    const RasterImage& image,
    const std::filesystem::path& destination)
{
    ComApartment apartment;
    if (!apartment.IsAvailable()) {
        return std::unexpected(L"无法初始化 Windows 图片组件。");
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void**>(factory.Put()));
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"创建 WIC 工厂", result));
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(stream.Put());
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"创建 PNG 文件流", result));
    }
    result = stream->InitializeFromFilename(destination.c_str(), GENERIC_WRITE);
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"创建 PNG 文件", result));
    }

    ComPtr<IWICBitmapEncoder> encoder;
    result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.Put());
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"创建 PNG 编码器", result));
    }
    result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"初始化 PNG 编码器", result));
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    result = encoder->CreateNewFrame(frame.Put(), nullptr);
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"创建 PNG 图像帧", result));
    }
    if (FAILED(result = frame->Initialize(nullptr)) ||
        FAILED(result = frame->SetSize(image.width, image.height))) {
        return std::unexpected(HResultMessage(L"初始化 PNG 图像帧", result));
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppBGRA;
    result = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(result) || pixelFormat != GUID_WICPixelFormat32bppBGRA) {
        return std::unexpected(L"PNG 编码器不支持 BGRA 像素格式。");
    }

    const UINT stride = image.width * 4U;
    result = frame->WritePixels(
        image.height,
        stride,
        static_cast<UINT>(image.bgra.size()),
        const_cast<BYTE*>(image.bgra.data()));
    if (FAILED(result) || FAILED(result = frame->Commit()) ||
        FAILED(result = encoder->Commit())) {
        return std::unexpected(HResultMessage(L"写入 PNG 文件", result));
    }
    return {};
}

std::expected<void, std::wstring> WriteSvg(
    const BinaryImage& lineArt,
    const std::filesystem::path& destination)
{
    if (lineArt.width == 0 || lineArt.height == 0 ||
        lineArt.pixels.size() !=
            static_cast<std::size_t>(lineArt.width) * lineArt.height) {
        return std::unexpected(L"线稿数据无效，无法创建 SVG 文件。");
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(L"无法创建 SVG 文件。");
    }
    output.imbue(std::locale::classic());
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    output << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 "
           << lineArt.width << ' ' << lineArt.height << "\" width=\"" << lineArt.width
           << "\" height=\"" << lineArt.height << "\">\n";
    output << "  <rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";
    output << "  <g fill=\"black\">\n";
    for (std::uint32_t y = 0; y < lineArt.height; ++y) {
        std::uint32_t x = 0;
        while (x < lineArt.width) {
            while (x < lineArt.width &&
                   lineArt.pixels[static_cast<std::size_t>(y) * lineArt.width + x] == 0) {
                ++x;
            }
            const std::uint32_t start = x;
            while (x < lineArt.width &&
                   lineArt.pixels[static_cast<std::size_t>(y) * lineArt.width + x] != 0) {
                ++x;
            }
            if (x > start) {
                output << "    <rect x=\"" << start << "\" y=\"" << y
                       << "\" width=\"" << (x - start) << "\" height=\"1\"/>\n";
            }
        }
    }
    output << "  </g>\n</svg>\n";
    if (!output) {
        return std::unexpected(L"写入 SVG 文件失败。");
    }
    return {};
}

} // namespace

std::expected<LineArtExportResult, std::wstring> ExportLineArt(
    const ProcessedImage& image,
    const std::filesystem::path& exportDirectory)
{
    std::error_code error;
    std::filesystem::create_directories(exportDirectory, error);
    if (error) {
        return std::unexpected(L"无法创建 exports 目录。");
    }

    std::wstring stem = image.sourcePath.stem().wstring();
    if (stem.empty()) {
        stem = L"image";
    }
    const std::wstring baseName = stem + L"-lineart";
    const std::wstring uniqueBaseName = UniqueBaseName(exportDirectory, baseName);
    const auto pngPath = exportDirectory / (uniqueBaseName + L".png");
    const auto svgPath = exportDirectory / (uniqueBaseName + L".svg");

    const std::uint32_t originalWidth = std::max(1U, image.originalWidth);
    const std::uint32_t originalHeight = std::max(1U, image.originalHeight);
    const double scale = std::min(
        1.0,
        4096.0 / static_cast<double>(std::max(originalWidth, originalHeight)));
    const auto outputWidth = static_cast<std::uint32_t>(
        std::max(1.0, std::round(static_cast<double>(originalWidth) * scale)));
    const auto outputHeight = static_cast<std::uint32_t>(
        std::max(1.0, std::round(static_cast<double>(originalHeight) * scale)));
    const RasterImage raster = RenderBinaryLineArt(
        image.lineArt.cleanLineArt, outputWidth, outputHeight);

    if (auto png = WritePng(raster, pngPath); !png) {
        return std::unexpected(png.error());
    }
    if (auto svg = WriteSvg(image.lineArt.cleanLineArt, svgPath); !svg) {
        std::filesystem::remove(pngPath, error);
        return std::unexpected(svg.error());
    }
    return LineArtExportResult{pngPath, svgPath};
}

} // namespace vrcdraw
