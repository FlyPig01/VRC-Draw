#include "LineArtExporter.hpp"

#include "PathMath.hpp"
#include "LineArtRenderer.hpp"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

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

struct ContourEndpoint {
    std::uint64_t key{};
    PointF point;
};

struct ContourSegment {
    ContourEndpoint first;
    ContourEndpoint second;
    bool used{};
};

struct ContourEndpointReference {
    std::uint64_t key{};
    std::size_t segment{};
};

std::uint64_t ContourEdgeKey(
    const bool vertical,
    const std::uint32_t x,
    const std::uint32_t y)
{
    return (static_cast<std::uint64_t>(y) << 33U) |
           (static_cast<std::uint64_t>(x) << 1U) |
           static_cast<std::uint64_t>(vertical);
}

std::vector<Stroke> TraceCoverageBoundaries(const GrayImage& lineArt)
{
    // Marching squares runs on a one-sample transparent border. Source samples
    // represent pixel centres, so an interpolated contour lies at sub-pixel
    // coordinates and follows the same coverage used by PNG and the preview.
    constexpr float threshold = 128.0F;
    const auto sample = [&](const std::uint32_t x, const std::uint32_t y) {
        if (x == 0 || y == 0 || x > lineArt.width || y > lineArt.height) {
            return std::uint8_t{};
        }
        return lineArt.pixels[
            static_cast<std::size_t>(y - 1U) * lineArt.width + (x - 1U)];
    };
    const auto interpolate = [](const std::uint8_t from, const std::uint8_t to) {
        const float denominator = static_cast<float>(to) - static_cast<float>(from);
        if (std::abs(denominator) < 0.001F) {
            return 0.5F;
        }
        return std::clamp(
            (threshold - static_cast<float>(from)) / denominator, 0.0F, 1.0F);
    };

    std::vector<ContourSegment> segments;
    segments.reserve(lineArt.pixels.size() / 3U);
    for (std::uint32_t y = 0; y <= lineArt.height; ++y) {
        for (std::uint32_t x = 0; x <= lineArt.width; ++x) {
            const std::array<std::uint8_t, 4> values{
                sample(x, y),
                sample(x + 1U, y),
                sample(x + 1U, y + 1U),
                sample(x, y + 1U),
            };
            const unsigned int state =
                (values[0] >= threshold ? 1U : 0U) |
                (values[1] >= threshold ? 2U : 0U) |
                (values[2] >= threshold ? 4U : 0U) |
                (values[3] >= threshold ? 8U : 0U);
            if (state == 0U || state == 15U) {
                continue;
            }
            const auto endpoint = [&](const int edge) {
                const float left = static_cast<float>(x) - 0.5F;
                const float top = static_cast<float>(y) - 0.5F;
                switch (edge) {
                case 0: {
                    const float ratio = interpolate(values[0], values[1]);
                    return ContourEndpoint{
                        ContourEdgeKey(false, x, y),
                        {std::clamp(left + ratio, 0.0F, static_cast<float>(lineArt.width)),
                         std::clamp(top, 0.0F, static_cast<float>(lineArt.height))},
                    };
                }
                case 1: {
                    const float ratio = interpolate(values[1], values[2]);
                    return ContourEndpoint{
                        ContourEdgeKey(true, x + 1U, y),
                        {std::clamp(left + 1.0F, 0.0F, static_cast<float>(lineArt.width)),
                         std::clamp(top + ratio, 0.0F, static_cast<float>(lineArt.height))},
                    };
                }
                case 2: {
                    const float ratio = interpolate(values[3], values[2]);
                    return ContourEndpoint{
                        ContourEdgeKey(false, x, y + 1U),
                        {std::clamp(left + ratio, 0.0F, static_cast<float>(lineArt.width)),
                         std::clamp(top + 1.0F, 0.0F, static_cast<float>(lineArt.height))},
                    };
                }
                default: {
                    const float ratio = interpolate(values[0], values[3]);
                    return ContourEndpoint{
                        ContourEdgeKey(true, x, y),
                        {std::clamp(left, 0.0F, static_cast<float>(lineArt.width)),
                         std::clamp(top + ratio, 0.0F, static_cast<float>(lineArt.height))},
                    };
                }
                }
            };
            const auto add = [&](const int first, const int second) {
                segments.push_back({endpoint(first), endpoint(second), false});
            };
            switch (state) {
            case 1: add(0, 3); break;
            case 2: add(0, 1); break;
            case 3: add(3, 1); break;
            case 4: add(1, 2); break;
            case 5: {
                const float centre = std::accumulate(
                    values.begin(), values.end(), 0.0F) / 4.0F;
                if (centre >= threshold) {
                    add(0, 1);
                    add(2, 3);
                } else {
                    add(0, 3);
                    add(1, 2);
                }
                break;
            }
            case 6: add(0, 2); break;
            case 7: add(2, 3); break;
            case 8: add(2, 3); break;
            case 9: add(0, 2); break;
            case 10: {
                const float centre = std::accumulate(
                    values.begin(), values.end(), 0.0F) / 4.0F;
                if (centre >= threshold) {
                    add(0, 3);
                    add(1, 2);
                } else {
                    add(0, 1);
                    add(2, 3);
                }
                break;
            }
            case 11: add(1, 2); break;
            case 12: add(3, 1); break;
            case 13: add(0, 1); break;
            case 14: add(0, 3); break;
            default: break;
            }
        }
    }

    std::vector<ContourEndpointReference> references;
    references.reserve(segments.size() * 2U);
    for (std::size_t index = 0; index < segments.size(); ++index) {
        references.push_back({segments[index].first.key, index});
        references.push_back({segments[index].second.key, index});
    }
    std::ranges::sort(references, {}, [](const ContourEndpointReference reference) {
        return reference.key;
    });
    const auto nextSegment = [&](const std::uint64_t key, const std::size_t current) {
        const auto lower = std::ranges::lower_bound(
            references, key, {}, [](const ContourEndpointReference reference) {
                return reference.key;
            });
        for (auto iterator = lower;
             iterator != references.end() && iterator->key == key;
             ++iterator) {
            if (iterator->segment != current && !segments[iterator->segment].used) {
                return iterator->segment;
            }
        }
        return segments.size();
    };

    std::vector<Stroke> loops;
    for (std::size_t startSegment = 0; startSegment < segments.size(); ++startSegment) {
        if (segments[startSegment].used) {
            continue;
        }
        Stroke loop;
        std::size_t segmentIndex = startSegment;
        const std::uint64_t startKey = segments[startSegment].first.key;
        std::uint64_t currentKey = startKey;
        loop.push_back(segments[startSegment].first.point);
        bool closed = false;
        for (std::size_t step = 0; step <= segments.size(); ++step) {
            ContourSegment& segment = segments[segmentIndex];
            if (segment.used) {
                break;
            }
            segment.used = true;
            const ContourEndpoint& destination = segment.first.key == currentKey
                ? segment.second
                : segment.first;
            if (destination.key == startKey) {
                closed = true;
                break;
            }
            loop.push_back(destination.point);
            const std::size_t next = nextSegment(destination.key, segmentIndex);
            if (next == segments.size()) {
                break;
            }
            currentKey = destination.key;
            segmentIndex = next;
        }
        if (closed && loop.size() >= 3) {
            loops.push_back(std::move(loop));
        }
    }
    return loops;
}

Stroke SimplifyClosedBoundary(const Stroke& loop)
{
    if (loop.size() < 8) {
        return loop;
    }
    const std::size_t first = static_cast<std::size_t>(std::distance(
        loop.begin(),
        std::ranges::min_element(loop, {}, [](const PointF point) {
            return std::pair{point.x, point.y};
        })));
    std::size_t opposite = first;
    float maximumDistance = 0.0F;
    for (std::size_t index = 0; index < loop.size(); ++index) {
        const float distance = std::hypot(
            loop[index].x - loop[first].x, loop[index].y - loop[first].y);
        if (distance > maximumDistance) {
            maximumDistance = distance;
            opposite = index;
        }
    }
    if (opposite == first) {
        return loop;
    }
    const auto collect = [&](const std::size_t begin, const std::size_t end) {
        Stroke part;
        for (std::size_t index = begin;; index = (index + 1U) % loop.size()) {
            part.push_back(loop[index]);
            if (index == end) {
                break;
            }
        }
        return SimplifyRdp(part, 0.35F);
    };
    Stroke firstHalf = collect(first, opposite);
    Stroke secondHalf = collect(opposite, first);
    Stroke result = std::move(firstHalf);
    if (secondHalf.size() > 2) {
        result.insert(result.end(), secondHalf.begin() + 1, secondHalf.end() - 1);
    }
    return result.size() >= 3 ? result : loop;
}

std::expected<void, std::wstring> WriteSvg(
    const GrayImage& lineArt,
    const std::filesystem::path& destination,
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight)
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
           << lineArt.width << ' ' << lineArt.height << "\" width=\"" << outputWidth
           << "\" height=\"" << outputHeight << "\">\n";
    output << "  <rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";
    output << "  <path fill=\"black\" fill-rule=\"evenodd\" "
              "shape-rendering=\"geometricPrecision\" d=\"";
    output << std::fixed << std::setprecision(2);
    for (const Stroke& rawLoop : TraceCoverageBoundaries(lineArt)) {
        const Stroke loop = SimplifyClosedBoundary(rawLoop);
        if (loop.size() < 3) {
            continue;
        }
        output << 'M' << loop.front().x << ' ' << loop.front().y;
        for (std::size_t index = 1; index < loop.size(); ++index) {
            output << 'L' << loop[index].x << ' ' << loop[index].y;
        }
        output << 'Z';
    }
    output << "\"/>\n</svg>\n";
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
    const RasterImage raster = RenderGrayscaleLineArt(
        image.lineArt.coverageLineArt, outputWidth, outputHeight);

    if (auto png = WritePng(raster, pngPath); !png) {
        return std::unexpected(png.error());
    }
    if (auto svg = WriteSvg(
            image.lineArt.coverageLineArt, svgPath, outputWidth, outputHeight);
        !svg) {
        std::filesystem::remove(pngPath, error);
        return std::unexpected(svg.error());
    }
    return LineArtExportResult{pngPath, svgPath};
}

} // namespace vrcdraw
