#include "ImageProcessor.hpp"

#include "PathMath.hpp"
#include "PathOptimizer.hpp"
#include "VectorPath.hpp"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <queue>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vrcdraw {
namespace {

constexpr int kMaximumSkeletonIterations = 192;

void ThinZhangSuen(
    std::vector<std::uint8_t>& image,
    std::uint32_t width,
    std::uint32_t height);

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept
    {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] T* Get() const { return value_; }
    [[nodiscard]] T** Put()
    {
        Reset();
        return &value_;
    }
    T* operator->() const { return value_; }

private:
    void Reset()
    {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

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

struct DecodedImage {
    std::uint32_t originalWidth{};
    std::uint32_t originalHeight{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> bgra;
};

std::wstring HResultMessage(const wchar_t* operation, const HRESULT result)
{
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"0x%08lX", static_cast<unsigned long>(result));
    return std::wstring(operation) + L"失败（" + buffer + L"）。";
}

std::expected<DecodedImage, std::wstring> DecodeAndResize(
    const std::filesystem::path& sourcePath,
    const ImageProcessingOptions& options)
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

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromFilename(
        sourcePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.Put());
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"打开图片", result));
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, frame.Put());
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"读取图片帧", result));
    }

    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    result = frame->GetSize(&sourceWidth, &sourceHeight);
    if (FAILED(result) || sourceWidth == 0 || sourceHeight == 0) {
        return std::unexpected(L"图片尺寸无效。");
    }

    const double dimensionScale =
        static_cast<double>(std::max(1U, options.maximumDimension)) /
        static_cast<double>(std::max(sourceWidth, sourceHeight));
    const double sourcePixels =
        static_cast<double>(sourceWidth) * static_cast<double>(sourceHeight);
    const double pixelScale = std::sqrt(
        static_cast<double>(std::max<std::uint64_t>(1, options.maximumPixels)) /
        sourcePixels);
    const double scale = std::min({1.0, dimensionScale, pixelScale});
    const auto targetWidth = static_cast<UINT>(std::max(1.0, std::round(sourceWidth * scale)));
    const auto targetHeight = static_cast<UINT>(std::max(1.0, std::round(sourceHeight * scale)));

    ComPtr<IWICBitmapSource> source;
    if (targetWidth != sourceWidth || targetHeight != sourceHeight) {
        ComPtr<IWICBitmapScaler> scaler;
        result = factory->CreateBitmapScaler(scaler.Put());
        if (FAILED(result)) {
            return std::unexpected(HResultMessage(L"创建图片缩放器", result));
        }
        result = scaler->Initialize(
            frame.Get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant);
        if (FAILED(result)) {
            return std::unexpected(HResultMessage(L"缩放图片", result));
        }
        IWICBitmapSource* scalerSource = nullptr;
        result = scaler->QueryInterface(
            IID_IWICBitmapSource, reinterpret_cast<void**>(&scalerSource));
        if (FAILED(result)) {
            return std::unexpected(HResultMessage(L"读取缩放结果", result));
        }
        *source.Put() = scalerSource;
    } else {
        IWICBitmapSource* frameSource = nullptr;
        result = frame->QueryInterface(
            IID_IWICBitmapSource, reinterpret_cast<void**>(&frameSource));
        if (FAILED(result)) {
            return std::unexpected(HResultMessage(L"读取图片数据", result));
        }
        *source.Put() = frameSource;
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(converter.Put());
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"创建图片格式转换器", result));
    }
    result = converter->Initialize(
        source.Get(),
        GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"转换图片像素格式", result));
    }

    const auto stride = targetWidth * 4U;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * targetHeight);
    result = converter->CopyPixels(
        nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(result)) {
        return std::unexpected(HResultMessage(L"复制图片像素", result));
    }

    return DecodedImage{
        sourceWidth, sourceHeight, targetWidth, targetHeight, std::move(pixels)};
}

std::uint8_t CompositeOnWhite(const std::uint8_t value, const std::uint8_t alpha)
{
    return static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(value) * alpha + 255U * (255U - alpha)) / 255U);
}

std::vector<std::uint8_t> ToGrayscale(const DecodedImage& image)
{
    std::vector<std::uint8_t> grayscale(
        static_cast<std::size_t>(image.width) * image.height);
    for (std::size_t pixel = 0; pixel < grayscale.size(); ++pixel) {
        const std::size_t offset = pixel * 4;
        const std::uint8_t alpha = image.bgra[offset + 3];
        const std::uint8_t blue = CompositeOnWhite(image.bgra[offset], alpha);
        const std::uint8_t green = CompositeOnWhite(image.bgra[offset + 1], alpha);
        const std::uint8_t red = CompositeOnWhite(image.bgra[offset + 2], alpha);
        grayscale[pixel] = static_cast<std::uint8_t>(
            (77U * red + 150U * green + 29U * blue) >> 8U);
    }
    return grayscale;
}

struct ColorPlanes {
    std::vector<std::uint8_t> red;
    std::vector<std::uint8_t> green;
    std::vector<std::uint8_t> blue;
};

ColorPlanes ToColorPlanes(const DecodedImage& image)
{
    const std::size_t size = static_cast<std::size_t>(image.width) * image.height;
    ColorPlanes planes{
        .red = std::vector<std::uint8_t>(size),
        .green = std::vector<std::uint8_t>(size),
        .blue = std::vector<std::uint8_t>(size),
    };
    for (std::size_t pixel = 0; pixel < size; ++pixel) {
        const std::size_t offset = pixel * 4;
        const std::uint8_t alpha = image.bgra[offset + 3];
        planes.blue[pixel] = CompositeOnWhite(image.bgra[offset], alpha);
        planes.green[pixel] = CompositeOnWhite(image.bgra[offset + 1], alpha);
        planes.red[pixel] = CompositeOnWhite(image.bgra[offset + 2], alpha);
    }
    return planes;
}

std::vector<float> GaussianBlur(
    const std::vector<std::uint8_t>& input,
    const std::uint32_t width,
    const std::uint32_t height,
    const float sigma)
{
    const int radius = std::max(1, static_cast<int>(std::ceil(sigma * 2.5F)));
    std::vector<float> kernel(static_cast<std::size_t>(radius * 2 + 1));
    float kernelSum = 0.0F;
    for (int offset = -radius; offset <= radius; ++offset) {
        const float value = std::exp(
            -static_cast<float>(offset * offset) / (2.0F * sigma * sigma));
        kernel[static_cast<std::size_t>(offset + radius)] = value;
        kernelSum += value;
    }
    for (float& value : kernel) {
        value /= kernelSum;
    }

    std::vector<float> horizontal(input.size());
    std::vector<float> output(input.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            float sum = 0.0F;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int sampleX = std::clamp(
                    static_cast<int>(x) + offset, 0, static_cast<int>(width) - 1);
                sum += static_cast<float>(input[static_cast<std::size_t>(y) * width +
                                                static_cast<std::size_t>(sampleX)]) *
                       kernel[static_cast<std::size_t>(offset + radius)];
            }
            horizontal[static_cast<std::size_t>(y) * width + x] = sum;
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            float sum = 0.0F;
            for (int offset = -radius; offset <= radius; ++offset) {
                const int sampleY = std::clamp(
                    static_cast<int>(y) + offset, 0, static_cast<int>(height) - 1);
                sum += horizontal[static_cast<std::size_t>(sampleY) * width + x] *
                       kernel[static_cast<std::size_t>(offset + radius)];
            }
            output[static_cast<std::size_t>(y) * width + x] = sum;
        }
    }
    return output;
}

int OtsuThreshold(const std::vector<std::uint8_t>& values)
{
    std::array<std::uint64_t, 256> histogram{};
    for (const std::uint8_t value : values) {
        ++histogram[value];
    }
    const std::uint64_t total = values.size();
    std::uint64_t weightedTotal = 0;
    for (std::size_t value = 0; value < histogram.size(); ++value) {
        weightedTotal += value * histogram[value];
    }

    std::uint64_t backgroundWeight = 0;
    std::uint64_t backgroundSum = 0;
    double bestVariance = -1.0;
    int bestThreshold = 128;
    for (int threshold = 0; threshold < 255; ++threshold) {
        backgroundWeight += histogram[static_cast<std::size_t>(threshold)];
        backgroundSum += static_cast<std::uint64_t>(threshold) *
                         histogram[static_cast<std::size_t>(threshold)];
        if (backgroundWeight == 0 || backgroundWeight == total) {
            continue;
        }
        const std::uint64_t foregroundWeight = total - backgroundWeight;
        const double backgroundMean = static_cast<double>(backgroundSum) /
                                      static_cast<double>(backgroundWeight);
        const double foregroundMean = static_cast<double>(weightedTotal - backgroundSum) /
                                      static_cast<double>(foregroundWeight);
        const double difference = backgroundMean - foregroundMean;
        const double variance = static_cast<double>(backgroundWeight) *
                                static_cast<double>(foregroundWeight) * difference * difference;
        if (variance > bestVariance) {
            bestVariance = variance;
            bestThreshold = threshold;
        }
    }
    return bestThreshold;
}

bool IsLikelyLineDrawing(const std::vector<std::uint8_t>& grayscale)
{
    const auto whitePixels = std::ranges::count_if(
        grayscale, [](const std::uint8_t value) { return value >= 238; });
    const auto darkPixels = std::ranges::count_if(
        grayscale, [](const std::uint8_t value) { return value <= 96; });
    const double size = static_cast<double>(std::max<std::size_t>(1, grayscale.size()));
    return static_cast<double>(whitePixels) / size >= 0.48 &&
           static_cast<double>(darkPixels) / size <= 0.32;
}

bool IsLikelyMonochromeLineDrawing(
    const DecodedImage& image,
    const std::vector<std::uint8_t>& grayscale)
{
    if (!IsLikelyLineDrawing(grayscale)) {
        return false;
    }
    std::size_t chromaticPixels = 0;
    for (std::size_t pixel = 0; pixel < grayscale.size(); ++pixel) {
        const std::size_t offset = pixel * 4;
        const std::uint8_t alpha = image.bgra[offset + 3];
        const std::uint8_t blue = CompositeOnWhite(image.bgra[offset], alpha);
        const std::uint8_t green = CompositeOnWhite(image.bgra[offset + 1], alpha);
        const std::uint8_t red = CompositeOnWhite(image.bgra[offset + 2], alpha);
        const std::uint8_t minimum = std::min({red, green, blue});
        const std::uint8_t maximum = std::max({red, green, blue});
        chromaticPixels += static_cast<int>(maximum) - static_cast<int>(minimum) >= 12;
    }
    const double size = static_cast<double>(std::max<std::size_t>(1, grayscale.size()));
    return static_cast<double>(chromaticPixels) / size <= 0.16;
}

bool IsLikelyFlatColorIllustration(
    const DecodedImage& image,
    const std::vector<std::uint8_t>& grayscale)
{
    std::array<std::uint8_t, 4096> quantizedColors{};
    std::size_t colorCount = 0;
    std::size_t backgroundLikePixels = 0;
    std::size_t chromaticPixels = 0;

    for (std::size_t pixel = 0; pixel < grayscale.size(); ++pixel) {
        const std::size_t offset = pixel * 4;
        const std::uint8_t alpha = image.bgra[offset + 3];
        const std::uint8_t blue = CompositeOnWhite(image.bgra[offset], alpha);
        const std::uint8_t green = CompositeOnWhite(image.bgra[offset + 1], alpha);
        const std::uint8_t red = CompositeOnWhite(image.bgra[offset + 2], alpha);
        const std::size_t bin = (static_cast<std::size_t>(red >> 4U) << 8U) |
                                (static_cast<std::size_t>(green >> 4U) << 4U) |
                                static_cast<std::size_t>(blue >> 4U);
        if (quantizedColors[bin] == 0) {
            quantizedColors[bin] = 1;
            ++colorCount;
        }

        backgroundLikePixels += grayscale[pixel] <= 32 || grayscale[pixel] >= 238;
        const std::uint8_t minimum = std::min({red, green, blue});
        const std::uint8_t maximum = std::max({red, green, blue});
        chromaticPixels += static_cast<int>(maximum) - static_cast<int>(minimum) >= 12;
    }

    const double size = static_cast<double>(std::max<std::size_t>(1, grayscale.size()));
    return colorCount <= 512 &&
           static_cast<double>(backgroundLikePixels) / size >= 0.18 &&
           static_cast<double>(chromaticPixels) / size >= 0.08;
}

void MergeInteriorDarkInk(
    std::vector<std::uint8_t>& inkMask,
    const std::vector<std::uint8_t>& grayscale,
    const std::uint32_t width,
    const std::uint32_t height)
{
    constexpr std::uint8_t maximumInkLuminance = 128;
    constexpr std::size_t maximumComponentSize = 1000;
    std::vector<std::uint8_t> visited(grayscale.size(), 0);
    std::vector<std::size_t> component;
    std::queue<std::size_t> pending;

    for (std::size_t start = 0; start < grayscale.size(); ++start) {
        if (visited[start] != 0 || grayscale[start] > maximumInkLuminance) {
            continue;
        }
        visited[start] = 1;
        pending.push(start);
        component.clear();
        bool touchesImageEdge = false;
        std::uint32_t minimumX = width;
        std::uint32_t minimumY = height;
        std::uint32_t maximumX = 0;
        std::uint32_t maximumY = 0;
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            component.push_back(current);
            const int x = static_cast<int>(current % width);
            const int y = static_cast<int>(current / width);
            minimumX = std::min(minimumX, static_cast<std::uint32_t>(x));
            minimumY = std::min(minimumY, static_cast<std::uint32_t>(y));
            maximumX = std::max(maximumX, static_cast<std::uint32_t>(x));
            maximumY = std::max(maximumY, static_cast<std::uint32_t>(y));
            touchesImageEdge = touchesImageEdge || x == 0 || y == 0 ||
                               x + 1 == static_cast<int>(width) ||
                               y + 1 == static_cast<int>(height);
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = x + offsetX;
                    const int neighborY = y + offsetY;
                    if ((offsetX == 0 && offsetY == 0) || neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(width) ||
                        neighborY >= static_cast<int>(height)) {
                        continue;
                    }
                    const auto neighbor = static_cast<std::size_t>(neighborY) * width +
                                          static_cast<std::size_t>(neighborX);
                    if (visited[neighbor] == 0 &&
                        grayscale[neighbor] <= maximumInkLuminance) {
                        visited[neighbor] = 1;
                        pending.push(neighbor);
                    }
                }
            }
        }

        const std::uint32_t componentWidth = maximumX - minimumX + 1U;
        const std::uint32_t componentHeight = maximumY - minimumY + 1U;
        const std::uint32_t longSide = std::max(componentWidth, componentHeight);
        const std::uint32_t shortSide = std::max(1U, std::min(componentWidth, componentHeight));
        if (!touchesImageEdge && component.size() >= 20 &&
            component.size() <= maximumComponentSize &&
            static_cast<float>(longSide) / static_cast<float>(shortSide) >= 2.0F) {
            for (const std::size_t index : component) {
                inkMask[index] = 1;
            }
        }
    }
}

[[maybe_unused]] void ReplaceInteriorDarkInkWithCenterlines(
    std::vector<std::uint8_t>& inkMask,
    const std::vector<std::uint8_t>& grayscale,
    const std::uint32_t width,
    const std::uint32_t height)
{
    constexpr std::uint8_t maximumInkLuminance = 128;
    std::vector<std::uint8_t> visited(grayscale.size(), 0);
    std::queue<std::size_t> pending;
    std::vector<std::size_t> component;

    for (std::size_t start = 0; start < grayscale.size(); ++start) {
        if (visited[start] != 0 || grayscale[start] > maximumInkLuminance) {
            continue;
        }
        visited[start] = 1;
        pending.push(start);
        component.clear();
        bool touchesImageEdge = false;
        double sumX = 0.0;
        double sumY = 0.0;
        std::uint32_t minimumX = width;
        std::uint32_t minimumY = height;
        std::uint32_t maximumX = 0;
        std::uint32_t maximumY = 0;
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            component.push_back(current);
            const int x = static_cast<int>(current % width);
            const int y = static_cast<int>(current / width);
            sumX += x;
            sumY += y;
            minimumX = std::min(minimumX, static_cast<std::uint32_t>(x));
            minimumY = std::min(minimumY, static_cast<std::uint32_t>(y));
            maximumX = std::max(maximumX, static_cast<std::uint32_t>(x));
            maximumY = std::max(maximumY, static_cast<std::uint32_t>(y));
            touchesImageEdge = touchesImageEdge || x == 0 || y == 0 ||
                               x + 1 == static_cast<int>(width) ||
                               y + 1 == static_cast<int>(height);
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = x + offsetX;
                    const int neighborY = y + offsetY;
                    if ((offsetX == 0 && offsetY == 0) || neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(width) ||
                        neighborY >= static_cast<int>(height)) {
                        continue;
                    }
                    const auto neighbor = static_cast<std::size_t>(neighborY) * width +
                                          static_cast<std::size_t>(neighborX);
                    if (visited[neighbor] == 0 &&
                        grayscale[neighbor] <= maximumInkLuminance) {
                        visited[neighbor] = 1;
                        pending.push(neighbor);
                    }
                }
            }
        }
        const std::size_t boundingArea =
            static_cast<std::size_t>(maximumX - minimumX + 1U) *
            static_cast<std::size_t>(maximumY - minimumY + 1U);
        if (touchesImageEdge || component.size() < 20 || component.size() > 1000 ||
            component.size() * 10U < boundingArea * 3U) {
            continue;
        }

        const double meanX = sumX / static_cast<double>(component.size());
        const double meanY = sumY / static_cast<double>(component.size());
        double xx = 0.0;
        double yy = 0.0;
        double xy = 0.0;
        for (const std::size_t index : component) {
            const double x = static_cast<double>(index % width) - meanX;
            const double y = static_cast<double>(index / width) - meanY;
            xx += x * x;
            yy += y * y;
            xy += x * y;
        }
        xx /= static_cast<double>(component.size());
        yy /= static_cast<double>(component.size());
        xy /= static_cast<double>(component.size());
        const double discriminant = std::sqrt(
            std::max(0.0, (xx - yy) * (xx - yy) + 4.0 * xy * xy));
        const double majorVariance = (xx + yy + discriminant) * 0.5;
        const double minorVariance = (xx + yy - discriminant) * 0.5;
        if (majorVariance < 16.0 || majorVariance < std::max(1.0, minorVariance) * 8.0) {
            continue;
        }

        double axisX = 1.0;
        double axisY = 0.0;
        if (std::abs(xy) > 1.0e-6) {
            axisX = xy;
            axisY = majorVariance - xx;
            const double length = std::hypot(axisX, axisY);
            axisX /= length;
            axisY /= length;
        } else if (yy > xx) {
            axisX = 0.0;
            axisY = 1.0;
        }

        double minimumProjection = std::numeric_limits<double>::infinity();
        double maximumProjection = -std::numeric_limits<double>::infinity();
        for (const std::size_t index : component) {
            const double x = static_cast<double>(index % width) - meanX;
            const double y = static_cast<double>(index / width) - meanY;
            const double projection = x * axisX + y * axisY;
            minimumProjection = std::min(minimumProjection, projection);
            maximumProjection = std::max(maximumProjection, projection);
        }
        const std::size_t binCount = static_cast<std::size_t>(
            std::ceil(maximumProjection - minimumProjection)) + 1U;
        if (binCount < 8) {
            continue;
        }
        std::vector<double> centerX(binCount, 0.0);
        std::vector<double> centerY(binCount, 0.0);
        std::vector<std::size_t> counts(binCount, 0);
        for (const std::size_t index : component) {
            const double x = static_cast<double>(index % width);
            const double y = static_cast<double>(index / width);
            const double projection =
                (x - meanX) * axisX + (y - meanY) * axisY;
            const std::size_t bin = std::min(
                binCount - 1U,
                static_cast<std::size_t>(std::lround(projection - minimumProjection)));
            centerX[bin] += x;
            centerY[bin] += y;
            ++counts[bin];
        }

        for (const std::size_t index : component) {
            const int x = static_cast<int>(index % width);
            const int y = static_cast<int>(index / width);
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int sampleX = x + offsetX;
                    const int sampleY = y + offsetY;
                    if (sampleX >= 0 && sampleY >= 0 &&
                        sampleX < static_cast<int>(width) &&
                        sampleY < static_cast<int>(height)) {
                        inkMask[static_cast<std::size_t>(sampleY) * width +
                                static_cast<std::size_t>(sampleX)] = 0;
                    }
                }
            }
        }

        PointF previous{};
        bool hasPrevious = false;
        const auto drawLine = [&](PointF from, const PointF to) {
            const int steps = std::max(
                1,
                static_cast<int>(std::ceil(std::max(
                    std::abs(to.x - from.x), std::abs(to.y - from.y)))));
            for (int step = 0; step <= steps; ++step) {
                const float ratio = static_cast<float>(step) / static_cast<float>(steps);
                const int x = static_cast<int>(std::lround(
                    from.x + (to.x - from.x) * ratio));
                const int y = static_cast<int>(std::lround(
                    from.y + (to.y - from.y) * ratio));
                if (x >= 0 && y >= 0 && x < static_cast<int>(width) &&
                    y < static_cast<int>(height)) {
                    inkMask[static_cast<std::size_t>(y) * width +
                            static_cast<std::size_t>(x)] = 1;
                }
            }
        };
        for (std::size_t bin = 0; bin < binCount; ++bin) {
            if (counts[bin] == 0) {
                continue;
            }
            const PointF center{
                static_cast<float>(centerX[bin] / static_cast<double>(counts[bin])),
                static_cast<float>(centerY[bin] / static_cast<double>(counts[bin])),
            };
            if (hasPrevious) {
                drawLine(previous, center);
            }
            previous = center;
            hasPrevious = true;
        }
    }
}

struct InkExtraction {
    std::vector<std::uint8_t> mask;
    bool lineDrawing{};
    bool flatColorIllustration{};
};

[[maybe_unused]] InkExtraction ExtractInkMask(
    const DecodedImage& image,
    const std::vector<std::uint8_t>& grayscale,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint8_t> mask(grayscale.size(), 0);
    if (IsLikelyLineDrawing(grayscale)) {
        const int threshold = std::clamp(OtsuThreshold(grayscale), 48, 224);
        for (std::size_t index = 0; index < grayscale.size(); ++index) {
            mask[index] = grayscale[index] <= threshold ? 1 : 0;
        }
        return InkExtraction{
            .mask = std::move(mask),
            .lineDrawing = true,
            .flatColorIllustration = false,
        };
    }

    // XDoG-style dark-ridge response. Unlike a gradient edge detector this marks the
    // dark stroke itself, so skeletonization produces one centre line rather than two edges.
    const bool flatColorIllustration = IsLikelyFlatColorIllustration(image, grayscale);
    const auto fine = GaussianBlur(grayscale, width, height, 0.8F);
    const auto coarse = GaussianBlur(
        grayscale, width, height, flatColorIllustration ? 3.0F : 1.6F);
    std::vector<std::uint8_t> response(grayscale.size(), 0);
    for (std::size_t index = 0; index < grayscale.size(); ++index) {
        const float darkRidge = std::max(0.0F, coarse[index] - fine[index]);
        response[index] = static_cast<std::uint8_t>(
            std::clamp(std::lround(darkRidge * 8.0F), 0L, 255L));
    }
    const int responseThreshold = std::clamp(OtsuThreshold(response), 12, 56);
    for (std::size_t index = 0; index < response.size(); ++index) {
        mask[index] = response[index] >= responseThreshold && fine[index] < 248.0F ? 1 : 0;
    }
    if (flatColorIllustration) {
        MergeInteriorDarkInk(mask, grayscale, width, height);
        constexpr std::uint32_t borderWidth = 4;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                if (x < borderWidth || y < borderWidth || x + borderWidth >= width ||
                    y + borderWidth >= height) {
                    mask[static_cast<std::size_t>(y) * width + x] = 0;
                }
            }
        }
    }
    return InkExtraction{
        .mask = std::move(mask),
        .lineDrawing = false,
        .flatColorIllustration = flatColorIllustration,
    };
}

std::vector<std::uint8_t> Dilate3x3(
    const std::vector<std::uint8_t>& input,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint8_t> output(input.size(), 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            bool foreground = false;
            for (int offsetY = -1; offsetY <= 1 && !foreground; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int sampleX = static_cast<int>(x) + offsetX;
                    const int sampleY = static_cast<int>(y) + offsetY;
                    if (sampleX >= 0 && sampleY >= 0 &&
                        sampleX < static_cast<int>(width) &&
                        sampleY < static_cast<int>(height) &&
                        input[static_cast<std::size_t>(sampleY) * width +
                              static_cast<std::size_t>(sampleX)] != 0) {
                        foreground = true;
                        break;
                    }
                }
            }
            output[static_cast<std::size_t>(y) * width + x] = foreground ? 1 : 0;
        }
    }
    return output;
}

std::vector<std::uint8_t> DilateSquare(
    const std::vector<std::uint8_t>& input,
    const std::uint32_t width,
    const std::uint32_t height,
    const int radius)
{
    if (radius <= 1) {
        return Dilate3x3(input, width, height);
    }
    std::vector<std::uint8_t> output(input.size(), 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            bool foreground = false;
            for (int offsetY = -radius; offsetY <= radius && !foreground; ++offsetY) {
                for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                    const int sampleX = static_cast<int>(x) + offsetX;
                    const int sampleY = static_cast<int>(y) + offsetY;
                    if (sampleX >= 0 && sampleY >= 0 &&
                        sampleX < static_cast<int>(width) &&
                        sampleY < static_cast<int>(height) &&
                        input[static_cast<std::size_t>(sampleY) * width +
                              static_cast<std::size_t>(sampleX)] != 0) {
                        foreground = true;
                        break;
                    }
                }
            }
            output[static_cast<std::size_t>(y) * width + x] = foreground ? 1 : 0;
        }
    }
    return output;
}

void RemoveSmallComponents(
    std::vector<std::uint8_t>& mask,
    const std::uint32_t width,
    const std::uint32_t height,
    const float processingScale = 1.0F)
{
    const std::size_t scaledMinimum = static_cast<std::size_t>(
        std::ceil(8.0F * processingScale * processingScale));
    const std::size_t minimumSize = std::max(scaledMinimum, mask.size() / 180000U);
    std::vector<std::uint8_t> visited(mask.size(), 0);
    std::vector<std::size_t> component;
    std::queue<std::size_t> pending;

    for (std::size_t start = 0; start < mask.size(); ++start) {
        if (mask[start] == 0 || visited[start] != 0) {
            continue;
        }
        component.clear();
        visited[start] = 1;
        pending.push(start);
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            component.push_back(current);
            const int x = static_cast<int>(current % width);
            const int y = static_cast<int>(current / width);
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (offsetX == 0 && offsetY == 0) {
                        continue;
                    }
                    const int neighborX = x + offsetX;
                    const int neighborY = y + offsetY;
                    if (neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(width) ||
                        neighborY >= static_cast<int>(height)) {
                        continue;
                    }
                    const auto neighbor = static_cast<std::size_t>(neighborY) * width +
                                          static_cast<std::size_t>(neighborX);
                    if (mask[neighbor] != 0 && visited[neighbor] == 0) {
                        visited[neighbor] = 1;
                        pending.push(neighbor);
                    }
                }
            }
        }
        if (component.size() < minimumSize) {
            for (const std::size_t index : component) {
                mask[index] = 0;
            }
        }
    }
}

void ClearImageBorder(
    std::vector<std::uint8_t>& mask,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t borderWidth = 8)
{
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (x < borderWidth || y < borderWidth || x + borderWidth >= width ||
                y + borderWidth >= height) {
                mask[static_cast<std::size_t>(y) * width + x] = 0;
            }
        }
    }
}

std::vector<std::uint8_t> ExtractColorBoundaries(
    const DecodedImage& image,
    const std::uint32_t width,
    const std::uint32_t height,
    const bool flatColorIllustration,
    const float processingScale)
{
    const auto planes = ToColorPlanes(image);
    const auto red = GaussianBlur(planes.red, width, height, 1.0F);
    const auto green = GaussianBlur(planes.green, width, height, 1.0F);
    const auto blue = GaussianBlur(planes.blue, width, height, 1.0F);
    std::vector<std::uint8_t> response(planes.red.size(), 0);
    std::vector<std::int8_t> directionX(planes.red.size(), 0);
    std::vector<std::int8_t> directionY(planes.red.size(), 0);

    for (std::uint32_t y = 1; y + 1 < height; ++y) {
        for (std::uint32_t x = 1; x + 1 < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const auto gradient = [&](const std::vector<float>& channel) {
                return std::pair{
                    channel[index + 1] - channel[index - 1],
                    channel[index + width] - channel[index - width]};
            };
            const auto [redX, redY] = gradient(red);
            const auto [greenX, greenY] = gradient(green);
            const auto [blueX, blueY] = gradient(blue);
            const float xx = redX * redX + greenX * greenX + blueX * blueX;
            const float yy = redY * redY + greenY * greenY + blueY * blueY;
            const float xy = redX * redY + greenX * greenY + blueX * blueY;
            const float discriminant = std::sqrt(
                std::max(0.0F, (xx - yy) * (xx - yy) + 4.0F * xy * xy));
            const float dominantGradient = std::sqrt(
                std::max(0.0F, (xx + yy + discriminant) / 6.0F));
            response[index] = static_cast<std::uint8_t>(
                std::clamp(std::lround(dominantGradient), 0L, 255L));
            const float angle = 0.5F * std::atan2(2.0F * xy, xx - yy);
            directionX[index] = static_cast<std::int8_t>(std::lround(std::cos(angle)));
            directionY[index] = static_cast<std::int8_t>(std::lround(std::sin(angle)));
        }
    }

    const int threshold = flatColorIllustration
        ? std::clamp(OtsuThreshold(response), 7, 20)
        : std::clamp(OtsuThreshold(response), 14, 36);
    const int continuationThreshold = std::max(4, threshold / 2);
    std::vector<std::uint8_t> localMaxima(response.size(), 0);
    const std::uint32_t boundaryMargin = static_cast<std::uint32_t>(
        std::max(4L, std::lround(4.0F * processingScale)));
    for (std::uint32_t y = boundaryMargin; y + boundaryMargin < height; ++y) {
        for (std::uint32_t x = boundaryMargin; x + boundaryMargin < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (response[index] < continuationThreshold) {
                continue;
            }
            const int stepX = directionX[index];
            const int stepY = directionY[index];
            if (stepX == 0 && stepY == 0) {
                continue;
            }
            const auto forward = static_cast<std::size_t>(
                static_cast<int>(index) + stepY * static_cast<int>(width) + stepX);
            const auto backward = static_cast<std::size_t>(
                static_cast<int>(index) - stepY * static_cast<int>(width) - stepX);
            localMaxima[index] =
                response[index] >= response[forward] && response[index] > response[backward]
                ? 1
                : 0;
        }
    }

    std::vector<std::uint8_t> boundaries(response.size(), 0);
    std::queue<std::size_t> pending;
    for (std::size_t index = 0; index < response.size(); ++index) {
        if (localMaxima[index] != 0 && response[index] >= threshold) {
            boundaries[index] = 1;
            pending.push(index);
        }
    }
    while (!pending.empty()) {
        const std::size_t current = pending.front();
        pending.pop();
        const int x = static_cast<int>(current % width);
        const int y = static_cast<int>(current / width);
        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                const int neighborX = x + offsetX;
                const int neighborY = y + offsetY;
                if ((offsetX == 0 && offsetY == 0) || neighborX < 0 || neighborY < 0 ||
                    neighborX >= static_cast<int>(width) ||
                    neighborY >= static_cast<int>(height)) {
                    continue;
                }
                const auto neighbor = static_cast<std::size_t>(neighborY) * width +
                                      static_cast<std::size_t>(neighborX);
                if (localMaxima[neighbor] != 0 && boundaries[neighbor] == 0) {
                    boundaries[neighbor] = 1;
                    pending.push(neighbor);
                }
            }
        }
    }
    return boundaries;
}

enum LineEvidenceFlags : std::uint8_t {
    kStrongCore = 1U << 0U,
    kOriginalDark = 1U << 1U,
    kRgbBoundary = 1U << 2U,
    kRepairedGap = 1U << 3U,
};

struct LinePixelEvidence {
    std::uint8_t confidence{};
    std::uint8_t tangentBin{255};
    std::uint8_t scaleMask{};
    std::uint8_t flags{};
};

struct CanonicalLineArtExtraction {
    GrayImage coverage;
    BinaryImage topology;
    std::vector<LinePixelEvidence> evidence;
};

std::uint8_t ResponsePercentile(
    const std::vector<std::uint8_t>& response,
    const std::vector<std::uint8_t>& support,
    const double percentile)
{
    std::array<std::uint64_t, 256> histogram{};
    std::uint64_t count = 0;
    for (std::size_t index = 0; index < response.size(); ++index) {
        if (support[index] != 0 && response[index] != 0) {
            ++histogram[response[index]];
            ++count;
        }
    }
    if (count == 0) {
        return 0;
    }
    const std::uint64_t target = static_cast<std::uint64_t>(std::clamp(
        std::ceil(static_cast<double>(count) * percentile), 1.0, static_cast<double>(count)));
    std::uint64_t cumulative = 0;
    for (std::size_t value = 1; value < histogram.size(); ++value) {
        cumulative += histogram[value];
        if (cumulative >= target) {
            return static_cast<std::uint8_t>(value);
        }
    }
    return 255;
}

struct PsLineResponse {
    std::vector<std::uint8_t> fused;
    std::array<std::vector<std::uint8_t>, 3> scales;
};

PsLineResponse BuildPsLineResponse(
    const std::vector<std::uint8_t>& grayscale,
    const std::uint32_t width,
    const std::uint32_t height,
    const float processingScale)
{
    const auto denoised = GaussianBlur(
        grayscale, width, height, std::max(0.55F, 0.55F * processingScale));
    PsLineResponse result{
        .fused = std::vector<std::uint8_t>(grayscale.size(), 0),
        .scales = {},
    };
    constexpr std::array<float, 3> baseSigmas{0.7F, 1.4F, 2.8F};
    constexpr std::array<float, 3> weights{1.0F, 1.0F, 0.72F};
    for (std::size_t scaleIndex = 0; scaleIndex < baseSigmas.size(); ++scaleIndex) {
        auto& scaleResponse = result.scales[scaleIndex];
        scaleResponse.resize(grayscale.size(), 0);
        const auto localMean = GaussianBlur(
            grayscale,
            width,
            height,
            std::max(0.65F, baseSigmas[scaleIndex] * processingScale));
        for (std::size_t index = 0; index < result.fused.size(); ++index) {
            const float denominator = std::max(1.0F, localMean[index]);
            const float colorDodgeInk = std::max(
                0.0F, 1.0F - denoised[index] / denominator);
            const auto value = static_cast<std::uint8_t>(std::clamp(
                std::lround(colorDodgeInk * 255.0F * 1.35F * weights[scaleIndex]),
                0L,
                255L));
            scaleResponse[index] = value;
            result.fused[index] = std::max(result.fused[index], value);
        }
    }
    return result;
}

void EstimateEvidenceTangents(
    std::vector<LinePixelEvidence>& evidence,
    const std::vector<std::uint8_t>& mask,
    const std::uint32_t width,
    const std::uint32_t height,
    const float processingScale)
{
    constexpr float pi = 3.14159265358979323846F;
    const int radius = std::max(2, static_cast<int>(std::lround(2.0F * processingScale)));
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (mask[index] == 0) {
                continue;
            }
            double weightSum = 0.0;
            double meanX = 0.0;
            double meanY = 0.0;
            for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
                for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                    const int sampleX = static_cast<int>(x) + offsetX;
                    const int sampleY = static_cast<int>(y) + offsetY;
                    if (sampleX < 0 || sampleY < 0 ||
                        sampleX >= static_cast<int>(width) ||
                        sampleY >= static_cast<int>(height)) {
                        continue;
                    }
                    const std::size_t sample =
                        static_cast<std::size_t>(sampleY) * width +
                        static_cast<std::size_t>(sampleX);
                    if (mask[sample] == 0) {
                        continue;
                    }
                    const double weight = 16.0 + evidence[sample].confidence;
                    weightSum += weight;
                    meanX += weight * sampleX;
                    meanY += weight * sampleY;
                }
            }
            if (weightSum <= 0.0) {
                continue;
            }
            meanX /= weightSum;
            meanY /= weightSum;
            double xx = 0.0;
            double yy = 0.0;
            double xy = 0.0;
            for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
                for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                    const int sampleX = static_cast<int>(x) + offsetX;
                    const int sampleY = static_cast<int>(y) + offsetY;
                    if (sampleX < 0 || sampleY < 0 ||
                        sampleX >= static_cast<int>(width) ||
                        sampleY >= static_cast<int>(height)) {
                        continue;
                    }
                    const std::size_t sample =
                        static_cast<std::size_t>(sampleY) * width +
                        static_cast<std::size_t>(sampleX);
                    if (mask[sample] == 0) {
                        continue;
                    }
                    const double weight = 16.0 + evidence[sample].confidence;
                    const double dx = sampleX - meanX;
                    const double dy = sampleY - meanY;
                    xx += weight * dx * dx;
                    yy += weight * dy * dy;
                    xy += weight * dx * dy;
                }
            }
            const double trace = xx + yy;
            const double discriminant = std::sqrt(
                std::max(0.0, (xx - yy) * (xx - yy) + 4.0 * xy * xy));
            if (trace <= 1.0e-5 || discriminant / trace < 0.18) {
                continue;
            }
            float angle = static_cast<float>(0.5 * std::atan2(2.0 * xy, xx - yy));
            if (angle < 0.0F) {
                angle += pi;
            }
            evidence[index].tangentBin = static_cast<std::uint8_t>(
                static_cast<int>(std::lround(angle * 16.0F / pi)) % 16);
        }
    }
}

std::vector<std::size_t> SkeletonNeighborsForRepair(
    const std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::size_t index)
{
    const int x = static_cast<int>(index % width);
    const int y = static_cast<int>(index / width);
    std::vector<std::size_t> result;
    result.reserve(8);
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            if (offsetX == 0 && offsetY == 0) {
                continue;
            }
            const int neighborX = x + offsetX;
            const int neighborY = y + offsetY;
            if (neighborX < 0 || neighborY < 0 ||
                neighborX >= static_cast<int>(width) ||
                neighborY >= static_cast<int>(height)) {
                continue;
            }
            const std::size_t neighbor =
                static_cast<std::size_t>(neighborY) * width +
                static_cast<std::size_t>(neighborX);
            if (skeleton[neighbor] == 0) {
                continue;
            }
            if (offsetX != 0 && offsetY != 0) {
                const std::size_t horizontal = static_cast<std::size_t>(y) * width +
                                               static_cast<std::size_t>(neighborX);
                const std::size_t vertical = static_cast<std::size_t>(neighborY) * width +
                                             static_cast<std::size_t>(x);
                if (skeleton[horizontal] != 0 || skeleton[vertical] != 0) {
                    continue;
                }
            }
            result.push_back(neighbor);
        }
    }
    return result;
}

PointF EndpointOutwardDirection(
    const std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::size_t endpoint,
    const std::size_t sampleLength)
{
    std::size_t previous = std::numeric_limits<std::size_t>::max();
    std::size_t current = endpoint;
    std::size_t farthest = endpoint;
    for (std::size_t step = 0; step < sampleLength; ++step) {
        auto neighbors = SkeletonNeighborsForRepair(skeleton, width, height, current);
        std::erase(neighbors, previous);
        if (neighbors.size() != 1) {
            break;
        }
        previous = current;
        current = neighbors.front();
        farthest = current;
    }
    const PointF direction{
        static_cast<float>(endpoint % width) - static_cast<float>(farthest % width),
        static_cast<float>(endpoint / width) - static_cast<float>(farthest / width),
    };
    const float length = std::hypot(direction.x, direction.y);
    return length >= 1.5F
        ? PointF{direction.x / length, direction.y / length}
        : PointF{};
}

void RemoveWeakOnePixelBridges(
    std::vector<std::uint8_t>& mask,
    const std::vector<std::uint8_t>& response,
    const std::uint32_t width,
    const std::uint32_t height)
{
    const auto support = DilateSquare(mask, width, height, 1);
    const int lowResponse = ResponsePercentile(response, support, 0.08);
    const int maximumBridgeResponse = std::max(3, lowResponse / 2);
    constexpr std::array<std::pair<int, int>, 8> ring{
        std::pair{0, -1},
        std::pair{1, -1},
        std::pair{1, 0},
        std::pair{1, 1},
        std::pair{0, 1},
        std::pair{-1, 1},
        std::pair{-1, 0},
        std::pair{-1, -1},
    };
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::size_t> remove;
        for (std::uint32_t y = 1; y + 1 < height; ++y) {
            for (std::uint32_t x = 1; x + 1 < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                if (mask[index] == 0 || response[index] > maximumBridgeResponse) {
                    continue;
                }
                std::array<std::uint8_t, 8> occupied{};
                int maximumNeighborResponse = 0;
                for (std::size_t neighbor = 0; neighbor < ring.size(); ++neighbor) {
                    const auto [offsetX, offsetY] = ring[neighbor];
                    const std::size_t sample = static_cast<std::size_t>(
                        static_cast<int>(y) + offsetY) * width +
                        static_cast<std::size_t>(static_cast<int>(x) + offsetX);
                    occupied[neighbor] = mask[sample] != 0 ? 1 : 0;
                    maximumNeighborResponse = std::max(
                        maximumNeighborResponse, static_cast<int>(response[sample]));
                }
                int groups = 0;
                int neighbors = 0;
                for (std::size_t neighbor = 0; neighbor < occupied.size(); ++neighbor) {
                    neighbors += occupied[neighbor];
                    groups += occupied[neighbor] != 0 &&
                              occupied[(neighbor + occupied.size() - 1) % occupied.size()] == 0;
                }
                if (neighbors >= 2 && groups >= 2 &&
                    maximumNeighborResponse >= static_cast<int>(response[index]) + 16) {
                    remove.push_back(index);
                }
            }
        }
        if (remove.empty()) {
            break;
        }
        for (const std::size_t index : remove) {
            mask[index] = 0;
        }
    }
}

void RepairDirectionalGaps(
    std::vector<std::uint8_t>& mask,
    const std::vector<std::uint8_t>& response,
    const std::uint32_t width,
    const std::uint32_t height,
    const float processingScale)
{
    auto skeleton = mask;
    ThinZhangSuen(skeleton, width, height);
    std::vector<std::size_t> endpoints;
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        if (skeleton[index] != 0 &&
            SkeletonNeighborsForRepair(skeleton, width, height, index).size() == 1) {
            endpoints.push_back(index);
        }
    }
    if (endpoints.size() < 2) {
        return;
    }

    std::vector<int> endpointByPixel(mask.size(), -1);
    std::vector<PointF> outward(endpoints.size());
    const std::size_t tangentSamples = static_cast<std::size_t>(std::max(
        4L, std::lround(6.0F * processingScale)));
    for (std::size_t endpoint = 0; endpoint < endpoints.size(); ++endpoint) {
        endpointByPixel[endpoints[endpoint]] = static_cast<int>(endpoint);
        outward[endpoint] = EndpointOutwardDirection(
            skeleton, width, height, endpoints[endpoint], tangentSamples);
    }

    const int maximumGap = std::max(2, static_cast<int>(std::lround(2.5F * processingScale)));
    const int responseThreshold = std::max(3, OtsuThreshold(response) / 4);
    std::vector<std::uint8_t> used(endpoints.size(), 0);
    for (std::size_t left = 0; left < endpoints.size(); ++left) {
        if (used[left] != 0 || (outward[left].x == 0.0F && outward[left].y == 0.0F)) {
            continue;
        }
        const int leftX = static_cast<int>(endpoints[left] % width);
        const int leftY = static_cast<int>(endpoints[left] / width);
        std::size_t best = endpoints.size();
        float bestDistance = std::numeric_limits<float>::infinity();
        for (int offsetY = -maximumGap; offsetY <= maximumGap; ++offsetY) {
            for (int offsetX = -maximumGap; offsetX <= maximumGap; ++offsetX) {
                const int x = leftX + offsetX;
                const int y = leftY + offsetY;
                if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
                    y >= static_cast<int>(height)) {
                    continue;
                }
                const int candidateValue = endpointByPixel[
                    static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
                if (candidateValue < 0) {
                    continue;
                }
                const std::size_t right = static_cast<std::size_t>(candidateValue);
                if (right <= left || used[right] != 0 ||
                    (outward[right].x == 0.0F && outward[right].y == 0.0F)) {
                    continue;
                }
                const float deltaX = static_cast<float>(x - leftX);
                const float deltaY = static_cast<float>(y - leftY);
                const float distance = std::hypot(deltaX, deltaY);
                if (distance < 1.5F || distance > static_cast<float>(maximumGap)) {
                    continue;
                }
                const PointF toward{deltaX / distance, deltaY / distance};
                const float leftAlignment =
                    outward[left].x * toward.x + outward[left].y * toward.y;
                const float rightAlignment =
                    -(outward[right].x * toward.x + outward[right].y * toward.y);
                if (leftAlignment < 0.82F || rightAlignment < 0.82F) {
                    continue;
                }

                const int steps = std::max(2, static_cast<int>(std::ceil(distance * 2.0F)));
                bool supported = true;
                for (int step = 1; step < steps; ++step) {
                    const float ratio = static_cast<float>(step) / static_cast<float>(steps);
                    const int sampleX = static_cast<int>(std::lround(
                        static_cast<float>(leftX) + deltaX * ratio));
                    const int sampleY = static_cast<int>(std::lround(
                        static_cast<float>(leftY) + deltaY * ratio));
                    const std::size_t sample = static_cast<std::size_t>(sampleY) * width +
                                               static_cast<std::size_t>(sampleX);
                    if (response[sample] < responseThreshold) {
                        supported = false;
                        break;
                    }
                }
                if (supported && distance < bestDistance) {
                    best = right;
                    bestDistance = distance;
                }
            }
        }
        if (best == endpoints.size()) {
            continue;
        }
        const int rightX = static_cast<int>(endpoints[best] % width);
        const int rightY = static_cast<int>(endpoints[best] / width);
        const int steps = std::max(std::abs(rightX - leftX), std::abs(rightY - leftY));
        for (int step = 0; step <= steps; ++step) {
            const float ratio = steps == 0
                ? 0.0F
                : static_cast<float>(step) / static_cast<float>(steps);
            const int x = static_cast<int>(std::lround(
                static_cast<float>(leftX) + static_cast<float>(rightX - leftX) * ratio));
            const int y = static_cast<int>(std::lround(
                static_cast<float>(leftY) + static_cast<float>(rightY - leftY) * ratio));
            mask[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = 1;
        }
        used[left] = 1;
        used[best] = 1;
    }
}

CanonicalLineArtExtraction ExtractCanonicalLineArt(
    const DecodedImage& image,
    const std::vector<std::uint8_t>& grayscale,
    const std::uint32_t width,
    const std::uint32_t height,
    const float processingScale,
    ImageProcessingDebug* const debug)
{
    std::vector<std::uint8_t> mask(grayscale.size(), 0);
    auto psResponse = BuildPsLineResponse(
        grayscale, width, height, processingScale);
    if (debug != nullptr) {
        debug->psResponse = GrayImage{width, height, psResponse.fused};
    }
    std::vector<std::uint8_t> response = std::move(psResponse.fused);
    std::vector<LinePixelEvidence> evidence(grayscale.size());
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        const int supportThreshold = std::max(
            4, static_cast<int>(std::lround(response[index] * 0.40F)));
        for (std::size_t scale = 0; scale < psResponse.scales.size(); ++scale) {
            if (psResponse.scales[scale][index] >= supportThreshold) {
                evidence[index].scaleMask |= static_cast<std::uint8_t>(1U << scale);
            }
        }
    }
    const auto fine = GaussianBlur(grayscale, width, height, 0.65F);
    const bool monochromeLineDrawing = IsLikelyMonochromeLineDrawing(image, grayscale);
    const bool flatColorIllustration =
        !monochromeLineDrawing && IsLikelyFlatColorIllustration(image, grayscale);
    if (monochromeLineDrawing) {
        const auto localMean = GaussianBlur(grayscale, width, height, 4.5F);
        const int globalThreshold = std::clamp(OtsuThreshold(grayscale), 96, 236);
        for (std::size_t index = 0; index < grayscale.size(); ++index) {
            const bool globallyDark = fine[index] <= static_cast<float>(globalThreshold);
            const bool locallyDark = fine[index] < 249.0F &&
                                     localMean[index] - fine[index] >= 3.5F;
            mask[index] = globallyDark || locallyDark ? 1 : 0;
            if (mask[index] != 0) {
                evidence[index].flags |= kOriginalDark;
            }
            const auto originalInk = static_cast<std::uint8_t>(std::clamp(
                std::lround((255.0F - fine[index]) * 1.15F), 0L, 255L));
            response[index] = std::max(response[index], originalInk);
        }
    } else {
        const auto localMean = GaussianBlur(
            grayscale,
            width,
            height,
            flatColorIllustration ? 4.0F : 3.2F);
        std::vector<std::uint8_t> ridgeResponse(grayscale.size(), 0);
        for (std::size_t index = 0; index < grayscale.size(); ++index) {
            const float darkRidge = std::max(0.0F, localMean[index] - fine[index]);
            ridgeResponse[index] = static_cast<std::uint8_t>(
                std::clamp(std::lround(darkRidge * 6.0F), 0L, 255L));
            response[index] = std::max(response[index], ridgeResponse[index]);
        }
        const int ridgeThreshold = flatColorIllustration
            ? std::clamp(OtsuThreshold(ridgeResponse), 8, 32)
            : std::clamp(OtsuThreshold(ridgeResponse), 12, 44);
        for (std::size_t index = 0; index < grayscale.size(); ++index) {
            mask[index] = ridgeResponse[index] >= ridgeThreshold && fine[index] < 249.0F
                ? 1
                : 0;
            if (mask[index] != 0) {
                evidence[index].flags |= kOriginalDark;
            }
        }

        constexpr int protectionRadius = 2;
        const auto protectedInk = DilateSquare(
            mask, width, height, protectionRadius);
        const auto colorBoundaries = ExtractColorBoundaries(
            image, width, height, flatColorIllustration, processingScale);
        for (std::size_t index = 0; index < mask.size(); ++index) {
            if (colorBoundaries[index] != 0 && protectedInk[index] == 0) {
                mask[index] = 1;
            }
            if (colorBoundaries[index] != 0) {
                response[index] = std::max(response[index], std::uint8_t{176});
                evidence[index].flags |= kRgbBoundary;
            }
        }
    }

    RemoveWeakOnePixelBridges(mask, response, width, height);
    const auto maskBeforeRepair = mask;
    RepairDirectionalGaps(mask, response, width, height, processingScale);
    for (std::size_t index = 0; index < mask.size(); ++index) {
        if (mask[index] != 0 && maskBeforeRepair[index] == 0) {
            evidence[index].flags |= kRepairedGap;
        }
    }
    RemoveSmallComponents(mask, width, height, processingScale);
    ClearImageBorder(
        mask,
        width,
        height,
        static_cast<std::uint32_t>(std::max(
            8L, std::lround(8.0F * processingScale))));

    const std::uint8_t strongThreshold = ResponsePercentile(response, mask, 0.58);
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        evidence[index].confidence = response[index];
        if (mask[index] != 0 && response[index] >= strongThreshold) {
            evidence[index].flags |= kStrongCore;
        }
        if (mask[index] == 0) {
            evidence[index].tangentBin = 255;
        }
    }
    EstimateEvidenceTangents(evidence, mask, width, height, processingScale);

    const auto displaySupport = DilateSquare(mask, width, height, 1);
    const std::uint8_t low = ResponsePercentile(response, displaySupport, 0.08);
    const std::uint8_t high = ResponsePercentile(response, displaySupport, 0.93);
    const float range = static_cast<float>(std::max(12, static_cast<int>(high) - low));
    std::vector<std::uint8_t> coverage(response.size(), 0);
    for (std::size_t index = 0; index < coverage.size(); ++index) {
        if (displaySupport[index] == 0) {
            continue;
        }
        const float normalized = std::clamp(
            (static_cast<float>(response[index]) - static_cast<float>(low)) / range,
            0.0F,
            1.0F);
        float adjusted = std::pow(normalized, 0.78F);
        if (mask[index] != 0) {
            // Keep the actual topology stroke dark. Intermediate coverage is
            // reserved for the sub-pixel fringe instead of making an entire
            // one-pixel stroke look washed out in the preview and PNG export.
            adjusted = std::max(adjusted, 0.82F);
        }
        coverage[index] = static_cast<std::uint8_t>(
            std::clamp(std::lround(adjusted * 255.0F), 0L, 255L));
    }
    const std::uint32_t borderWidth = static_cast<std::uint32_t>(std::max(
        8L, std::lround(8.0F * processingScale)));
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            if (x < borderWidth || y < borderWidth || x + borderWidth >= width ||
                y + borderWidth >= height) {
                coverage[static_cast<std::size_t>(y) * width + x] = 0;
            }
        }
    }
    return CanonicalLineArtExtraction{
        .coverage = GrayImage{width, height, std::move(coverage)},
        .topology = BinaryImage{width, height, std::move(mask)},
        .evidence = std::move(evidence),
    };
}

int BinaryTransitions(const std::array<std::uint8_t, 8>& neighbors)
{
    int transitions = 0;
    for (std::size_t index = 0; index < neighbors.size(); ++index) {
        transitions += neighbors[index] == 0 &&
                       neighbors[(index + 1) % neighbors.size()] != 0;
    }
    return transitions;
}

void ThinZhangSuen(
    std::vector<std::uint8_t>& image,
    const std::uint32_t width,
    const std::uint32_t height)
{
    if (width < 3 || height < 3) {
        return;
    }
    std::vector<std::size_t> remove;
    remove.reserve(image.size() / 16);

    for (int iteration = 0; iteration < kMaximumSkeletonIterations; ++iteration) {
        bool changed = false;
        for (int substep = 0; substep < 2; ++substep) {
            remove.clear();
            for (std::uint32_t y = 1; y + 1 < height; ++y) {
                for (std::uint32_t x = 1; x + 1 < width; ++x) {
                    const auto index = static_cast<std::size_t>(y) * width + x;
                    if (image[index] == 0) {
                        continue;
                    }
                    const std::array<std::uint8_t, 8> p{
                        image[index - width],
                        image[index - width + 1],
                        image[index + 1],
                        image[index + width + 1],
                        image[index + width],
                        image[index + width - 1],
                        image[index - 1],
                        image[index - width - 1],
                    };
                    const int count = std::accumulate(p.begin(), p.end(), 0);
                    if (count < 2 || count > 6 || BinaryTransitions(p) != 1) {
                        continue;
                    }
                    const bool firstTriplet = substep == 0
                        ? p[0] * p[2] * p[4] == 0
                        : p[0] * p[2] * p[6] == 0;
                    const bool secondTriplet = substep == 0
                        ? p[2] * p[4] * p[6] == 0
                        : p[0] * p[4] * p[6] == 0;
                    if (firstTriplet && secondTriplet) {
                        remove.push_back(index);
                    }
                }
            }
            for (const std::size_t index : remove) {
                image[index] = 0;
            }
            changed = changed || !remove.empty();
        }
        if (!changed) {
            break;
        }
    }
}

std::vector<std::size_t> PixelNeighbors(
    const std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::size_t index)
{
    const int x = static_cast<int>(index % width);
    const int y = static_cast<int>(index / width);
    std::vector<std::size_t> result;
    result.reserve(8);
    for (int offsetY = -1; offsetY <= 1; ++offsetY) {
        for (int offsetX = -1; offsetX <= 1; ++offsetX) {
            if (offsetX == 0 && offsetY == 0) {
                continue;
            }
            const int neighborX = x + offsetX;
            const int neighborY = y + offsetY;
            if (neighborX < 0 || neighborY < 0 ||
                neighborX >= static_cast<int>(width) ||
                neighborY >= static_cast<int>(height)) {
                continue;
            }
            const auto neighbor = static_cast<std::size_t>(neighborY) * width +
                                  static_cast<std::size_t>(neighborX);
            if (skeleton[neighbor] == 0) {
                continue;
            }
            if (offsetX != 0 && offsetY != 0) {
                const auto horizontal = static_cast<std::size_t>(y) * width +
                                        static_cast<std::size_t>(neighborX);
                const auto vertical = static_cast<std::size_t>(neighborY) * width +
                                      static_cast<std::size_t>(x);
                if (skeleton[horizontal] != 0 || skeleton[vertical] != 0) {
                    continue;
                }
            }
            result.push_back(neighbor);
        }
    }
    return result;
}

[[maybe_unused]] void PruneShortSpurs(
    std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::size_t maximumSpurLength)
{
    for (int pass = 0; pass < 3; ++pass) {
        std::vector<std::size_t> toRemove;
        for (std::size_t start = 0; start < skeleton.size(); ++start) {
            if (skeleton[start] == 0 ||
                PixelNeighbors(skeleton, width, height, start).size() != 1) {
                continue;
            }
            std::vector<std::size_t> branch{start};
            std::size_t previous = std::numeric_limits<std::size_t>::max();
            std::size_t current = start;
            bool reachedJunction = false;
            while (branch.size() <= maximumSpurLength) {
                auto neighbors = PixelNeighbors(skeleton, width, height, current);
                std::erase(neighbors, previous);
                if (neighbors.size() != 1) {
                    reachedJunction = neighbors.size() > 1;
                    break;
                }
                previous = current;
                current = neighbors.front();
                const auto degree = PixelNeighbors(skeleton, width, height, current).size();
                if (degree > 2) {
                    reachedJunction = true;
                    break;
                }
                branch.push_back(current);
            }
            if (reachedJunction && branch.size() <= maximumSpurLength) {
                toRemove.insert(toRemove.end(), branch.begin(), branch.end());
            }
        }
        if (toRemove.empty()) {
            break;
        }
        for (const std::size_t index : toRemove) {
            skeleton[index] = 0;
        }
    }
}

[[maybe_unused]] void CollapseSmallElongatedBranches(
    std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint8_t> visited(skeleton.size(), 0);
    std::queue<std::size_t> pending;
    std::vector<std::size_t> component;
    std::vector<std::size_t> endpoints;

    for (std::size_t start = 0; start < skeleton.size(); ++start) {
        if (skeleton[start] == 0 || visited[start] != 0) {
            continue;
        }
        visited[start] = 1;
        pending.push(start);
        component.clear();
        endpoints.clear();
        std::uint32_t minimumX = width;
        std::uint32_t minimumY = height;
        std::uint32_t maximumX = 0;
        std::uint32_t maximumY = 0;
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            component.push_back(current);
            const auto x = static_cast<std::uint32_t>(current % width);
            const auto y = static_cast<std::uint32_t>(current / width);
            minimumX = std::min(minimumX, x);
            minimumY = std::min(minimumY, y);
            maximumX = std::max(maximumX, x);
            maximumY = std::max(maximumY, y);
            const auto neighbors = PixelNeighbors(skeleton, width, height, current);
            if (neighbors.size() == 1) {
                endpoints.push_back(current);
            }
            for (const std::size_t neighbor : neighbors) {
                if (visited[neighbor] == 0) {
                    visited[neighbor] = 1;
                    pending.push(neighbor);
                }
            }
        }

        const std::uint32_t componentWidth = maximumX - minimumX + 1;
        const std::uint32_t componentHeight = maximumY - minimumY + 1;
        const std::uint32_t longSide = std::max(componentWidth, componentHeight);
        const std::uint32_t shortSide = std::max(1U, std::min(componentWidth, componentHeight));
        if (endpoints.size() < 3 || component.size() > 1500 ||
            longSide > std::max(width, height) / 6U ||
            static_cast<float>(longSide) / static_cast<float>(shortSide) < 1.6F) {
            continue;
        }

        const auto farthestEndpoint = [&](
                                          const std::size_t source,
                                          std::vector<std::size_t>* predecessor) {
            std::vector<int> distance(skeleton.size(), -1);
            if (predecessor != nullptr) {
                predecessor->assign(skeleton.size(), std::numeric_limits<std::size_t>::max());
            }
            std::queue<std::size_t> search;
            distance[source] = 0;
            search.push(source);
            while (!search.empty()) {
                const std::size_t current = search.front();
                search.pop();
                for (const std::size_t neighbor :
                     PixelNeighbors(skeleton, width, height, current)) {
                    if (distance[neighbor] >= 0) {
                        continue;
                    }
                    distance[neighbor] = distance[current] + 1;
                    if (predecessor != nullptr) {
                        (*predecessor)[neighbor] = current;
                    }
                    search.push(neighbor);
                }
            }
            return *std::ranges::max_element(
                endpoints,
                {},
                [&](const std::size_t endpoint) { return distance[endpoint]; });
        };

        const std::size_t first = farthestEndpoint(endpoints.front(), nullptr);
        std::vector<std::size_t> predecessor;
        const std::size_t last = farthestEndpoint(first, &predecessor);
        std::vector<std::size_t> longestPath;
        for (std::size_t current = last;
             current != std::numeric_limits<std::size_t>::max();
             current = predecessor[current]) {
            longestPath.push_back(current);
            if (current == first) {
                break;
            }
        }
        if (longestPath.empty() || longestPath.back() != first) {
            continue;
        }
        for (const std::size_t index : component) {
            skeleton[index] = 0;
        }
        for (const std::size_t index : longestPath) {
            skeleton[index] = 1;
        }
    }
}

std::uint64_t EdgeKey(const std::size_t left, const std::size_t right)
{
    const auto minimum = static_cast<std::uint32_t>(std::min(left, right));
    const auto maximum = static_cast<std::uint32_t>(std::max(left, right));
    return (static_cast<std::uint64_t>(minimum) << 32U) | maximum;
}

float StrokeLength(const Stroke& stroke)
{
    float length = 0.0F;
    for (std::size_t index = 1; index < stroke.size(); ++index) {
        length += std::hypot(
            stroke[index].x - stroke[index - 1].x,
            stroke[index].y - stroke[index - 1].y);
    }
    return length;
}

[[maybe_unused]] Stroke SmoothStroke(const Stroke& stroke)
{
    if (stroke.size() < 4) {
        return stroke;
    }
    Stroke result = stroke;
    for (std::size_t index = 1; index + 1 < stroke.size(); ++index) {
        result[index] = PointF{
            stroke[index - 1].x * 0.2F + stroke[index].x * 0.6F +
                stroke[index + 1].x * 0.2F,
            stroke[index - 1].y * 0.2F + stroke[index].y * 0.6F +
                stroke[index + 1].y * 0.2F,
        };
    }
    return result;
}

float DirectionCosine(
    const PointF leftStart,
    const PointF leftEnd,
    const PointF rightStart,
    const PointF rightEnd)
{
    const float leftX = leftEnd.x - leftStart.x;
    const float leftY = leftEnd.y - leftStart.y;
    const float rightX = rightEnd.x - rightStart.x;
    const float rightY = rightEnd.y - rightStart.y;
    const float denominator = std::hypot(leftX, leftY) * std::hypot(rightX, rightY);
    if (denominator <= 1.0e-5F) {
        return -1.0F;
    }
    return (leftX * rightX + leftY * rightY) / denominator;
}

bool IsClosedStroke(const Stroke& stroke)
{
    return stroke.size() >= 4 &&
           std::hypot(
               stroke.front().x - stroke.back().x,
               stroke.front().y - stroke.back().y) <= 1.5F;
}

struct StrokeMergeCandidate {
    std::size_t left{};
    std::size_t right{};
    bool reverseLeft{};
    bool reverseRight{};
    float score{std::numeric_limits<float>::infinity()};
};

[[maybe_unused]] std::vector<Stroke> MergeAlignedStrokes(
    std::vector<Stroke> strokes,
    const float maximumGap)
{
    for (;;) {
        StrokeMergeCandidate best{};
        bool found = false;

        for (std::size_t leftIndex = 0; leftIndex < strokes.size(); ++leftIndex) {
            if (strokes[leftIndex].size() < 2 || IsClosedStroke(strokes[leftIndex])) {
                continue;
            }
            for (std::size_t rightIndex = leftIndex + 1; rightIndex < strokes.size(); ++rightIndex) {
                if (strokes[rightIndex].size() < 2 || IsClosedStroke(strokes[rightIndex])) {
                    continue;
                }
                for (const bool reverseLeft : {false, true}) {
                    const Stroke& left = strokes[leftIndex];
                    const PointF leftEnd = reverseLeft ? left.front() : left.back();
                    const PointF leftBefore = reverseLeft ? left[1] : left[left.size() - 2];

                    for (const bool reverseRight : {false, true}) {
                        const Stroke& right = strokes[rightIndex];
                        const PointF rightStart = reverseRight ? right.back() : right.front();
                        const PointF rightAfter = reverseRight
                            ? right[right.size() - 2]
                            : right[1];
                        const float gap = std::hypot(
                            rightStart.x - leftEnd.x,
                            rightStart.y - leftEnd.y);
                        if (gap > maximumGap) {
                            continue;
                        }

                        const float continuation = DirectionCosine(
                            leftBefore, leftEnd, rightStart, rightAfter);
                        if (continuation < 0.55F) {
                            continue;
                        }
                        if (gap > 0.75F) {
                            const float intoGap = DirectionCosine(
                                leftBefore, leftEnd, leftEnd, rightStart);
                            const float outOfGap = DirectionCosine(
                                leftEnd, rightStart, rightStart, rightAfter);
                            if (intoGap < 0.72F || outOfGap < 0.72F) {
                                continue;
                            }
                        }

                        const float score = gap + (1.0F - continuation) * 2.0F;
                        if (!found || score < best.score) {
                            best = StrokeMergeCandidate{
                                .left = leftIndex,
                                .right = rightIndex,
                                .reverseLeft = reverseLeft,
                                .reverseRight = reverseRight,
                                .score = score,
                            };
                            found = true;
                        }
                    }
                }
            }
        }

        if (!found) {
            break;
        }

        Stroke& left = strokes[best.left];
        Stroke& right = strokes[best.right];
        if (best.reverseLeft) {
            std::ranges::reverse(left);
        }
        if (best.reverseRight) {
            std::ranges::reverse(right);
        }
        const float gap = std::hypot(
            right.front().x - left.back().x,
            right.front().y - left.back().y);
        left.reserve(left.size() + right.size());
        left.insert(
            left.end(),
            right.begin() + static_cast<std::ptrdiff_t>(gap <= 0.25F),
            right.end());
        strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(best.right));
    }
    return strokes;
}

std::vector<float> InsideInkDistance(
    const std::vector<std::uint8_t>& mask,
    const std::uint32_t width,
    const std::uint32_t height)
{
    constexpr float diagonal = 1.41421356F;
    constexpr float infinity = 1.0e6F;
    std::vector<float> distance(mask.size(), infinity);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (mask[index] == 0) {
                distance[index] = 0.0F;
            } else if (x == 0 || y == 0 || x + 1 == width || y + 1 == height) {
                distance[index] = 1.0F;
            }
        }
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (x > 0) {
                distance[index] = std::min(distance[index], distance[index - 1] + 1.0F);
            }
            if (y > 0) {
                distance[index] = std::min(distance[index], distance[index - width] + 1.0F);
                if (x > 0) {
                    distance[index] = std::min(
                        distance[index], distance[index - width - 1] + diagonal);
                }
                if (x + 1 < width) {
                    distance[index] = std::min(
                        distance[index], distance[index - width + 1] + diagonal);
                }
            }
        }
    }
    for (std::uint32_t y = height; y-- > 0;) {
        for (std::uint32_t x = width; x-- > 0;) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (x + 1 < width) {
                distance[index] = std::min(distance[index], distance[index + 1] + 1.0F);
            }
            if (y + 1 < height) {
                distance[index] = std::min(distance[index], distance[index + width] + 1.0F);
                if (x > 0) {
                    distance[index] = std::min(
                        distance[index], distance[index + width - 1] + diagonal);
                }
                if (x + 1 < width) {
                    distance[index] = std::min(
                        distance[index], distance[index + width + 1] + diagonal);
                }
            }
        }
    }
    return distance;
}

std::vector<std::uint8_t> RegionBoundary(
    const std::vector<std::uint8_t>& region,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint8_t> boundary(region.size(), 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            if (region[index] == 0) {
                continue;
            }
            bool edge = x == 0 || y == 0 || x + 1 == width || y + 1 == height;
            for (int offsetY = -1; offsetY <= 1 && !edge; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = static_cast<int>(x) + offsetX;
                    const int neighborY = static_cast<int>(y) + offsetY;
                    if (neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(width) ||
                        neighborY >= static_cast<int>(height) ||
                        region[static_cast<std::size_t>(neighborY) * width +
                               static_cast<std::size_t>(neighborX)] == 0) {
                        edge = true;
                        break;
                    }
                }
            }
            boundary[index] = edge ? 1 : 0;
        }
    }
    return boundary;
}

struct RouteSkeletonData {
    std::vector<std::uint8_t> skeleton;
    std::vector<std::uint8_t> regionTypes;
    std::vector<float> inkDistance;
    std::size_t compactRegionCount{};
    std::size_t elongatedRegionCount{};
};

RouteSkeletonData BuildRouteSkeleton(
    const std::vector<std::uint8_t>& sourceInk,
    const std::vector<LinePixelEvidence>& evidence,
    const std::uint32_t width,
    const std::uint32_t height,
    const float processingScale)
{
    auto skeleton = sourceInk;
    ThinZhangSuen(skeleton, width, height);
    auto inkDistance = InsideInkDistance(sourceInk, width, height);
    std::vector<std::uint8_t> regionTypes(
        sourceInk.size(), static_cast<std::uint8_t>(LineRegionType::ThinLine));
    std::vector<std::uint8_t> thickCore(sourceInk.size(), 0);
    const float seedRadius = std::max(3.5F, 2.75F * processingScale);
    for (std::size_t index = 0; index < sourceInk.size(); ++index) {
        const bool reliable = index < evidence.size() &&
            ((evidence[index].flags & (kStrongCore | kOriginalDark | kRgbBoundary)) != 0 ||
             evidence[index].confidence >= 48);
        thickCore[index] = sourceInk[index] != 0 && reliable &&
                           inkDistance[index] >= seedRadius
            ? 1
            : 0;
    }

    std::vector<std::uint8_t> visitedCore(sourceInk.size(), 0);
    std::vector<int> expansionDistance(sourceInk.size(), -1);
    std::vector<std::uint8_t> compactRegion(sourceInk.size(), 0);
    std::vector<std::uint8_t> regionMembership(sourceInk.size(), 0);
    std::queue<std::size_t> pending;
    std::vector<std::size_t> coreComponent;
    std::vector<std::size_t> region;
    std::vector<std::size_t> touched;
    std::size_t compactRegionCount = 0;
    std::size_t elongatedRegionCount = 0;
    const std::size_t minimumCoreSize = static_cast<std::size_t>(
        std::ceil(seedRadius * seedRadius * 0.70F));

    for (std::size_t start = 0; start < thickCore.size(); ++start) {
        if (thickCore[start] == 0 || visitedCore[start] != 0) {
            continue;
        }
        coreComponent.clear();
        visitedCore[start] = 1;
        pending.push(start);
        float maximumRadius = inkDistance[start];
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            coreComponent.push_back(current);
            maximumRadius = std::max(maximumRadius, inkDistance[current]);
            const int x = static_cast<int>(current % width);
            const int y = static_cast<int>(current / width);
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = x + offsetX;
                    const int neighborY = y + offsetY;
                    if ((offsetX == 0 && offsetY == 0) || neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(width) ||
                        neighborY >= static_cast<int>(height)) {
                        continue;
                    }
                    const std::size_t neighbor =
                        static_cast<std::size_t>(neighborY) * width +
                        static_cast<std::size_t>(neighborX);
                    if (thickCore[neighbor] != 0 && visitedCore[neighbor] == 0) {
                        visitedCore[neighbor] = 1;
                        pending.push(neighbor);
                    }
                }
            }
        }
        if (coreComponent.size() < minimumCoreSize) {
            continue;
        }

        region.clear();
        touched.clear();
        const int maximumExpansion = static_cast<int>(std::ceil(
            maximumRadius + 2.0F * processingScale));
        for (const std::size_t pixel : coreComponent) {
            expansionDistance[pixel] = 0;
            touched.push_back(pixel);
            pending.push(pixel);
        }
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            region.push_back(current);
            if (expansionDistance[current] >= maximumExpansion) {
                continue;
            }
            const int x = static_cast<int>(current % width);
            const int y = static_cast<int>(current / width);
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = x + offsetX;
                    const int neighborY = y + offsetY;
                    if ((offsetX == 0 && offsetY == 0) || neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(width) ||
                        neighborY >= static_cast<int>(height)) {
                        continue;
                    }
                    const std::size_t neighbor =
                        static_cast<std::size_t>(neighborY) * width +
                        static_cast<std::size_t>(neighborX);
                    if (sourceInk[neighbor] != 0 && expansionDistance[neighbor] < 0) {
                        expansionDistance[neighbor] = expansionDistance[current] + 1;
                        touched.push_back(neighbor);
                        pending.push(neighbor);
                    }
                }
            }
        }
        for (const std::size_t pixel : touched) {
            expansionDistance[pixel] = -1;
        }
        if (region.empty()) {
            continue;
        }
        for (const std::size_t pixel : region) {
            regionMembership[pixel] = 1;
        }

        double meanX = 0.0;
        double meanY = 0.0;
        int minimumX = static_cast<int>(width);
        int maximumX = 0;
        int minimumY = static_cast<int>(height);
        int maximumY = 0;
        std::size_t skeletonPixels = 0;
        std::vector<std::size_t> portPixels;
        for (const std::size_t pixel : region) {
            const int x = static_cast<int>(pixel % width);
            const int y = static_cast<int>(pixel / width);
            meanX += x;
            meanY += y;
            minimumX = std::min(minimumX, x);
            maximumX = std::max(maximumX, x);
            minimumY = std::min(minimumY, y);
            maximumY = std::max(maximumY, y);
            skeletonPixels += skeleton[pixel] != 0;
            if (skeleton[pixel] == 0) {
                continue;
            }
            for (const std::size_t neighbor : PixelNeighbors(
                     skeleton, width, height, pixel)) {
                if (regionMembership[neighbor] == 0) {
                    portPixels.push_back(neighbor);
                }
            }
        }
        std::ranges::sort(portPixels);
        portPixels.erase(std::unique(portPixels.begin(), portPixels.end()), portPixels.end());
        std::vector<std::uint8_t> visitedPorts(portPixels.size(), 0);
        std::size_t portCount = 0;
        std::vector<PointF> portCentroids;
        const int portMergeDistance = std::max(
            2, static_cast<int>(std::lround(2.0F * processingScale)));
        for (std::size_t port = 0; port < portPixels.size(); ++port) {
            if (visitedPorts[port] != 0) {
                continue;
            }
            ++portCount;
            visitedPorts[port] = 1;
            std::queue<std::size_t> portQueue;
            portQueue.push(port);
            double portSumX = 0.0;
            double portSumY = 0.0;
            std::size_t portPixelCount = 0;
            while (!portQueue.empty()) {
                const std::size_t current = portQueue.front();
                portQueue.pop();
                const int currentX = static_cast<int>(portPixels[current] % width);
                const int currentY = static_cast<int>(portPixels[current] / width);
                portSumX += currentX;
                portSumY += currentY;
                ++portPixelCount;
                for (std::size_t candidate = 0; candidate < portPixels.size(); ++candidate) {
                    if (visitedPorts[candidate] != 0) {
                        continue;
                    }
                    const int candidateX = static_cast<int>(portPixels[candidate] % width);
                    const int candidateY = static_cast<int>(portPixels[candidate] / width);
                    if (std::max(
                            std::abs(candidateX - currentX),
                            std::abs(candidateY - currentY)) <= portMergeDistance) {
                        visitedPorts[candidate] = 1;
                        portQueue.push(candidate);
                    }
                }
            }
            portCentroids.push_back(PointF{
                static_cast<float>(portSumX / static_cast<double>(portPixelCount)),
                static_cast<float>(portSumY / static_cast<double>(portPixelCount)),
            });
        }
        meanX /= static_cast<double>(region.size());
        meanY /= static_cast<double>(region.size());
        double xx = 0.0;
        double yy = 0.0;
        double xy = 0.0;
        for (const std::size_t pixel : region) {
            const double dx = static_cast<double>(pixel % width) - meanX;
            const double dy = static_cast<double>(pixel / width) - meanY;
            xx += dx * dx;
            yy += dy * dy;
            xy += dx * dy;
        }
        const double discriminant = std::sqrt(
            std::max(0.0, (xx - yy) * (xx - yy) + 4.0 * xy * xy));
        const double major = std::max(1.0, (xx + yy + discriminant) * 0.5);
        const double minor = std::max(1.0, (xx + yy - discriminant) * 0.5);
        const double covarianceAspect = std::sqrt(major / minor);
        const double boxAspect = static_cast<double>(
            std::max(maximumX - minimumX + 1, maximumY - minimumY + 1)) /
            static_cast<double>(std::max(
                1, std::min(maximumX - minimumX + 1, maximumY - minimumY + 1)));
        const bool loopLikeStroke = portCount <= 2 &&
            static_cast<double>(skeletonPixels) >=
                std::sqrt(static_cast<double>(region.size())) * 2.25;
        bool twoPortContinuation = false;
        if (portCentroids.size() == 2) {
            const PointF left{
                portCentroids[0].x - static_cast<float>(meanX),
                portCentroids[0].y - static_cast<float>(meanY),
            };
            const PointF right{
                portCentroids[1].x - static_cast<float>(meanX),
                portCentroids[1].y - static_cast<float>(meanY),
            };
            const float denominator = std::hypot(left.x, left.y) *
                                      std::hypot(right.x, right.y);
            twoPortContinuation = denominator > 1.0e-5F &&
                (left.x * right.x + left.y * right.y) / denominator <= -0.35F;
        }
        const bool elongated = covarianceAspect >= 4.0 || boxAspect >= 5.0 ||
                               loopLikeStroke || twoPortContinuation;
        const auto type = portCount >= 3
            ? LineRegionType::Junction
            : (elongated
                ? LineRegionType::ElongatedThickStroke
                : LineRegionType::CompactFill);
        if (type == LineRegionType::ElongatedThickStroke) {
            ++elongatedRegionCount;
        } else if (type == LineRegionType::CompactFill) {
            ++compactRegionCount;
        }
        for (const std::size_t pixel : region) {
            const auto existing = static_cast<LineRegionType>(regionTypes[pixel]);
            const bool replace = type == LineRegionType::Junction ||
                (type == LineRegionType::ElongatedThickStroke &&
                 (existing == LineRegionType::ThinLine ||
                  existing == LineRegionType::CompactFill)) ||
                (type == LineRegionType::CompactFill &&
                 existing == LineRegionType::ThinLine);
            if (replace) {
                regionTypes[pixel] = static_cast<std::uint8_t>(type);
            }
            regionMembership[pixel] = 0;
        }
    }

    for (std::size_t pixel = 0; pixel < compactRegion.size(); ++pixel) {
        compactRegion[pixel] = regionTypes[pixel] ==
                static_cast<std::uint8_t>(LineRegionType::CompactFill)
            ? 1
            : 0;
    }

    auto compactBoundary = RegionBoundary(compactRegion, width, height);
    ThinZhangSuen(compactBoundary, width, height);
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        if (compactRegion[index] != 0) {
            skeleton[index] = 0;
        }
        skeleton[index] = skeleton[index] != 0 || compactBoundary[index] != 0 ? 1 : 0;
    }
    return RouteSkeletonData{
        .skeleton = std::move(skeleton),
        .regionTypes = std::move(regionTypes),
        .inkDistance = std::move(inkDistance),
        .compactRegionCount = compactRegionCount,
        .elongatedRegionCount = elongatedRegionCount,
    };
}

struct TracedRoute {
    std::vector<Stroke> strokes;
    std::vector<StrokeRouteMetadata> metadata;
};

TracedRoute TraceSkeleton(
    const std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::vector<std::uint8_t>& sourceInk,
    const std::vector<LinePixelEvidence>& evidence,
    const std::vector<std::uint8_t>& regionTypes,
    const std::vector<float>& inkDistance,
    const float processingScale,
    ImageProcessingDebug* const debug)
{
    std::vector<std::vector<std::size_t>> pixelAdjacency(skeleton.size());
    std::size_t pixelEdgeCount = 0;
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        if (skeleton[index] == 0) {
            continue;
        }
        pixelAdjacency[index] = PixelNeighbors(skeleton, width, height, index);
        pixelEdgeCount += pixelAdjacency[index].size();
    }

    struct SkeletonNode {
        std::vector<std::size_t> pixels;
        PointF point;
        bool junction{};
    };
    struct GraphEdge {
        std::size_t left{};
        std::size_t right{};
        Stroke points;
        float length{};
        float confidence{};
        float lowConfidenceRatio{};
        float endpointConfidence{};
        float multiScaleSupport{};
        float originalSupport{};
        float repairedSupport{};
        LineRegionType regionType{LineRegionType::ThinLine};
    };

    const std::size_t missing = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> nodeByPixel(skeleton.size(), missing);
    std::vector<SkeletonNode> nodes;
    std::vector<std::vector<std::size_t>> graphAdjacency;
    std::vector<GraphEdge> graphEdges;
    std::unordered_set<std::uint64_t> visitedEdges;
    visitedEdges.reserve(pixelEdgeCount / 2 + 1);

    // A thinned crossing is commonly a several-pixel blob. Build one non-overlapping
    // junction zone before extracting components: independently expanding already
    // separated high-degree components can assign the same bridge pixel to two nodes
    // and sever one branch of an acute crossing. One skeleton hop is enough to join
    // such a split core without restoring the old image-scaled junction merge.
    std::vector<std::uint8_t> junctionCore(skeleton.size(), 0);
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        junctionCore[index] = pixelAdjacency[index].size() >= 3 ? 1 : 0;
    }
    std::vector<std::uint8_t> junctionZone = junctionCore;
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        // A shallow crossing often thins into two three-port cores joined by a
        // degree-two bridge. The stage-two region classifier provides the missing
        // evidence: absorb only skeleton pixels inside the same Junction region.
        // This is topology/evidence gated, not a distance merge, so two crossings
        // connected by an ordinary thin stroke remain distinct.
        if (skeleton[index] != 0 && index < regionTypes.size() &&
            regionTypes[index] == static_cast<std::uint8_t>(LineRegionType::Junction) &&
            pixelAdjacency[index].size() >= 2) {
            junctionZone[index] = 1;
        }
        if (junctionCore[index] == 0) {
            continue;
        }
        for (const std::size_t neighbor : pixelAdjacency[index]) {
            // Keep degree-one pixels outside the zone. They may be genuine short
            // eyelashes or other intentional terminal details.
            if (pixelAdjacency[neighbor].size() >= 2) {
                junctionZone[neighbor] = 1;
            }
        }
    }
    std::vector<std::uint8_t> visitedCandidates(skeleton.size(), 0);
    for (std::size_t start = 0; start < skeleton.size(); ++start) {
        if (junctionZone[start] == 0 || visitedCandidates[start] != 0) {
            continue;
        }

        std::vector<std::size_t> region;
        std::queue<std::size_t> pending;
        visitedCandidates[start] = 1;
        pending.push(start);
        while (!pending.empty()) {
            const std::size_t current = pending.front();
            pending.pop();
            region.push_back(current);
            for (const std::size_t neighbor : pixelAdjacency[current]) {
                if (junctionZone[neighbor] != 0 && visitedCandidates[neighbor] == 0) {
                    visitedCandidates[neighbor] = 1;
                    pending.push(neighbor);
                }
            }
        }
        if (std::ranges::none_of(
                region,
                [&](const std::size_t pixel) { return junctionCore[pixel] != 0; })) {
            continue;
        }

        const std::size_t nodeIndex = nodes.size();
        double sumX = 0.0;
        double sumY = 0.0;
        for (const std::size_t pixel : region) {
            nodeByPixel[pixel] = nodeIndex;
            sumX += static_cast<double>(pixel % width);
            sumY += static_cast<double>(pixel / width);
        }
        const double count = static_cast<double>(std::max<std::size_t>(1, region.size()));
        const PointF centroid{
            static_cast<float>(sumX / count),
            static_cast<float>(sumY / count),
        };
        const std::size_t representativePixel = *std::ranges::min_element(
            region,
            {},
            [&](const std::size_t pixel) {
                return std::hypot(
                    static_cast<float>(pixel % width) - centroid.x,
                    static_cast<float>(pixel / width) - centroid.y);
            });
        nodes.push_back(SkeletonNode{
            .pixels = std::move(region),
            .point = PointF{
                static_cast<float>(representativePixel % width),
                static_cast<float>(representativePixel / width),
            },
            .junction = true,
        });
        graphAdjacency.emplace_back();
    }

    // Endpoints are single-pixel nodes. Isolated pixels are retained as nodes as well,
    // although a mouse path needs at least two distinct pixels before it can be drawn.
    for (std::size_t pixel = 0; pixel < skeleton.size(); ++pixel) {
        if (skeleton[pixel] == 0 || nodeByPixel[pixel] != missing ||
            pixelAdjacency[pixel].size() == 2) {
            continue;
        }
        const std::size_t nodeIndex = nodes.size();
        nodeByPixel[pixel] = nodeIndex;
        nodes.push_back(SkeletonNode{
            .pixels = {pixel},
            .point = PointF{
                static_cast<float>(pixel % width),
                static_cast<float>(pixel / width),
            },
            .junction = pixelAdjacency[pixel].size() >= 3,
        });
        graphAdjacency.emplace_back();
    }

    const auto pointPixel = [&](const PointF point) {
        const int x = std::clamp(
            static_cast<int>(std::lround(point.x)), 0, static_cast<int>(width) - 1);
        const int y = std::clamp(
            static_cast<int>(std::lround(point.y)), 0, static_cast<int>(height) - 1);
        return static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
    };
    if (debug != nullptr) {
        debug->junctionRegions = GrayImage{
            .width = width,
            .height = height,
            .pixels = std::vector<std::uint8_t>(skeleton.size(), 0),
        };
        debug->junctionCount = 0;
        for (const SkeletonNode& node : nodes) {
            if (!node.junction) {
                continue;
            }
            ++debug->junctionCount;
            for (const std::size_t pixel : node.pixels) {
                debug->junctionRegions.pixels[pixel] = 255;
            }
        }
    }

    // Internal edges of a junction blob are geometry noise, not drawable branches.
    for (std::size_t pixel = 0; pixel < skeleton.size(); ++pixel) {
        if (nodeByPixel[pixel] == missing) {
            continue;
        }
        for (const std::size_t neighbor : pixelAdjacency[pixel]) {
            if (nodeByPixel[neighbor] == nodeByPixel[pixel]) {
                visitedEdges.insert(EdgeKey(pixel, neighbor));
            }
        }
    }

    const auto appendGraphEdge = [&](const std::size_t left,
                                     const std::size_t startPixel,
                                     const std::size_t firstNeighbor) {
        // Preserve the actual skeleton pixel at the boundary of the merged node.
        // Using the node centroid here creates an unvalidated chord through the
        // junction blob and can cross white space when the blob is large.
        Stroke points{PointF{
            static_cast<float>(startPixel % width),
            static_cast<float>(startPixel / width),
        }};
        std::size_t previous = startPixel;
        std::size_t current = firstNeighbor;
        visitedEdges.insert(EdgeKey(previous, current));
        std::size_t right = missing;

        for (;;) {
            if (nodeByPixel[current] != missing) {
                right = nodeByPixel[current];
                points.push_back(PointF{
                    static_cast<float>(current % width),
                    static_cast<float>(current / width),
                });
                break;
            }
            points.push_back(PointF{
                static_cast<float>(current % width),
                static_cast<float>(current / width),
            });
            const auto& neighbors = pixelAdjacency[current];
            if (neighbors.size() != 2) {
                break;
            }
            const std::size_t next = neighbors[0] == previous ? neighbors[1] : neighbors[0];
            if (visitedEdges.contains(EdgeKey(current, next))) {
                break;
            }
            visitedEdges.insert(EdgeKey(current, next));
            previous = current;
            current = next;
        }

        if (right == missing || points.size() < 2) {
            return;
        }
        const std::size_t edgeIndex = graphEdges.size();
        graphEdges.push_back(GraphEdge{
            .left = left,
            .right = right,
            .points = std::move(points),
        });
        graphEdges.back().length = StrokeLength(graphEdges.back().points);
        double confidenceSum = 0.0;
        std::size_t multiScalePoints = 0;
        std::size_t originalPoints = 0;
        std::size_t repairedPoints = 0;
        std::size_t lowConfidencePoints = 0;
        std::array<std::size_t, 5> typeCounts{};
        for (const PointF point : graphEdges.back().points) {
            const int x = std::clamp(
                static_cast<int>(std::lround(point.x)), 0, static_cast<int>(width) - 1);
            const int y = std::clamp(
                static_cast<int>(std::lround(point.y)), 0, static_cast<int>(height) - 1);
            const std::size_t pixel = static_cast<std::size_t>(y) * width +
                                      static_cast<std::size_t>(x);
            if (pixel < evidence.size()) {
                confidenceSum += evidence[pixel].confidence;
                lowConfidencePoints += evidence[pixel].confidence < 64;
                multiScalePoints += std::popcount(evidence[pixel].scaleMask) >= 2;
                originalPoints += (evidence[pixel].flags &
                                   (kOriginalDark | kRgbBoundary)) != 0;
                repairedPoints += (evidence[pixel].flags & kRepairedGap) != 0;
            }
            if (pixel < regionTypes.size() && regionTypes[pixel] < typeCounts.size()) {
                ++typeCounts[regionTypes[pixel]];
            }
        }
        const float pointCount = static_cast<float>(
            std::max<std::size_t>(1, graphEdges.back().points.size()));
        graphEdges.back().confidence = static_cast<float>(confidenceSum) /
                                       (255.0F * pointCount);
        graphEdges.back().lowConfidenceRatio =
            static_cast<float>(lowConfidencePoints) / pointCount;
        const auto endpointConfidence = [&](const PointF point) {
            const int x = std::clamp(
                static_cast<int>(std::lround(point.x)), 0, static_cast<int>(width) - 1);
            const int y = std::clamp(
                static_cast<int>(std::lround(point.y)), 0, static_cast<int>(height) - 1);
            const std::size_t pixel = static_cast<std::size_t>(y) * width +
                                      static_cast<std::size_t>(x);
            return pixel < evidence.size()
                ? static_cast<float>(evidence[pixel].confidence) / 255.0F
                : 0.0F;
        };
        graphEdges.back().endpointConfidence = std::min(
            endpointConfidence(graphEdges.back().points.front()),
            endpointConfidence(graphEdges.back().points.back()));
        graphEdges.back().multiScaleSupport =
            static_cast<float>(multiScalePoints) / pointCount;
        graphEdges.back().originalSupport = static_cast<float>(originalPoints) / pointCount;
        graphEdges.back().repairedSupport = static_cast<float>(repairedPoints) / pointCount;
        graphEdges.back().regionType = static_cast<LineRegionType>(
            std::distance(typeCounts.begin(), std::ranges::max_element(typeCounts)));
        graphAdjacency[left].push_back(edgeIndex);
        if (right != left) {
            graphAdjacency[right].push_back(edgeIndex);
        }
    };

    for (std::size_t nodeIndex = 0; nodeIndex < nodes.size(); ++nodeIndex) {
        for (const std::size_t pixel : nodes[nodeIndex].pixels) {
            for (const std::size_t neighbor : pixelAdjacency[pixel]) {
                if (nodeByPixel[neighbor] != nodeIndex &&
                    !visitedEdges.contains(EdgeKey(pixel, neighbor))) {
                    appendGraphEdge(nodeIndex, pixel, neighbor);
                }
            }
        }
    }

    // A closed contour has no endpoint or junction. Trace every remaining pixel edge
    // explicitly so rings and small enclosed details cannot disappear.
    std::vector<Stroke> closedStrokes;
    for (std::size_t pixel = 0; pixel < skeleton.size(); ++pixel) {
        if (skeleton[pixel] == 0) {
            continue;
        }
        for (const std::size_t neighbor : pixelAdjacency[pixel]) {
            if (visitedEdges.contains(EdgeKey(pixel, neighbor))) {
                continue;
            }
            Stroke loop{
                PointF{
                    static_cast<float>(pixel % width),
                    static_cast<float>(pixel / width),
                },
            };
            std::size_t previous = pixel;
            std::size_t current = neighbor;
            visitedEdges.insert(EdgeKey(previous, current));
            while (current != pixel) {
                loop.push_back(PointF{
                    static_cast<float>(current % width),
                    static_cast<float>(current / width),
                });
                const auto& neighbors = pixelAdjacency[current];
                if (neighbors.size() != 2) {
                    break;
                }
                const std::size_t next =
                    neighbors[0] == previous ? neighbors[1] : neighbors[0];
                if (visitedEdges.contains(EdgeKey(current, next))) {
                    break;
                }
                visitedEdges.insert(EdgeKey(current, next));
                previous = current;
                current = next;
            }
            if (current == pixel) {
                loop.push_back(loop.front());
            }
            if (loop.size() >= 2) {
                closedStrokes.push_back(std::move(loop));
            }
        }
    }

    // Keep route geometry within the actual line-art mask. A wider tolerance at
    // 1536 px allowed fitted junction segments to bridge narrow white gaps and was
    // visible as strokes outside the preview, so this safety margin deliberately
    // remains one raster pixel at every processing scale.
    constexpr int inkSupportRadius = 1;
    const auto supportedByInk = [&](const PointF point) {
        const int centerX = static_cast<int>(std::lround(point.x));
        const int centerY = static_cast<int>(std::lround(point.y));
        for (int offsetY = -inkSupportRadius; offsetY <= inkSupportRadius; ++offsetY) {
            for (int offsetX = -inkSupportRadius; offsetX <= inkSupportRadius; ++offsetX) {
                const int x = centerX + offsetX;
                const int y = centerY + offsetY;
                if (x >= 0 && y >= 0 && x < static_cast<int>(width) &&
                    y < static_cast<int>(height) &&
                    sourceInk[static_cast<std::size_t>(y) * width +
                              static_cast<std::size_t>(x)] != 0) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto segmentSupportedByInk = [&](const PointF start, const PointF end) {
        const int steps = std::max(
            1,
            static_cast<int>(std::ceil(std::hypot(end.x - start.x, end.y - start.y) * 2.0F)));
        for (int step = 0; step <= steps; ++step) {
            const float ratio = static_cast<float>(step) / static_cast<float>(steps);
            if (!supportedByInk(PointF{
                    start.x + (end.x - start.x) * ratio,
                    start.y + (end.y - start.y) * ratio,
                })) {
                return false;
            }
        }
        return true;
    };
    const auto strokeSupportedByInk = [&](const Stroke& stroke) {
        return stroke.size() >= 2 &&
            std::ranges::all_of(
                std::views::iota(std::size_t{1}, stroke.size()),
                [&](const std::size_t index) {
                    return segmentSupportedByInk(stroke[index - 1], stroke[index]);
                });
    };
    const auto simplifyWithinInk = [&](Stroke stroke) {
        if (stroke.size() <= 2) {
            return stroke;
        }
        Stroke simplified = SimplifyRdp(stroke, 0.35F * processingScale);
        return strokeSupportedByInk(simplified) ? std::move(simplified) : std::move(stroke);
    };
    const auto localInkRadius = [&](const PointF point) {
        const int centerX = static_cast<int>(std::lround(point.x));
        const int centerY = static_cast<int>(std::lround(point.y));
        if (centerX >= 0 && centerY >= 0 && centerX < static_cast<int>(width) &&
            centerY < static_cast<int>(height)) {
            const std::size_t index = static_cast<std::size_t>(centerY) * width +
                                      static_cast<std::size_t>(centerX);
            if (index < inkDistance.size() && inkDistance[index] < 1.0e5F) {
                return std::max(1.0F, inkDistance[index]);
            }
        }
        const int maximumRadius = std::max(
            1, static_cast<int>(std::lround(16.0F * processingScale)));
        for (int radius = 1; radius <= maximumRadius; ++radius) {
            for (int offset = -radius; offset <= radius; ++offset) {
                for (const auto [x, y] : std::array{
                         std::pair{centerX + offset, centerY - radius},
                         std::pair{centerX + offset, centerY + radius},
                         std::pair{centerX - radius, centerY + offset},
                         std::pair{centerX + radius, centerY + offset},
                     }) {
                    if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
                        y >= static_cast<int>(height) ||
                        sourceInk[static_cast<std::size_t>(y) * width +
                                  static_cast<std::size_t>(x)] == 0) {
                        return static_cast<float>(radius);
                    }
                }
            }
        }
        return static_cast<float>(maximumRadius);
    };

    // Small source components can collapse to one skeleton pixel. They still carry
    // visible image information, so represent each one with the longest tiny segment
    // that stays inside its source ink instead of silently dropping it.
    std::vector<Stroke> isolatedStrokes;
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!graphAdjacency[node].empty() || nodes[node].pixels.empty()) {
            continue;
        }
        const PointF start = nodes[node].point;
        PointF end = start;
        float bestLength = 0.0F;
        const int centerX = static_cast<int>(std::lround(start.x));
        const int centerY = static_cast<int>(std::lround(start.y));
        const int isolatedSearchRadius = std::max(
            1, static_cast<int>(std::lround(4.0F * processingScale)));
        for (int offsetY = -isolatedSearchRadius;
             offsetY <= isolatedSearchRadius;
             ++offsetY) {
            for (int offsetX = -isolatedSearchRadius;
                 offsetX <= isolatedSearchRadius;
                 ++offsetX) {
                const int x = centerX + offsetX;
                const int y = centerY + offsetY;
                if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
                    y >= static_cast<int>(height) ||
                    sourceInk[static_cast<std::size_t>(y) * width +
                              static_cast<std::size_t>(x)] == 0) {
                    continue;
                }
                const PointF candidate{
                    static_cast<float>(x),
                    static_cast<float>(y),
                };
                const float length = std::hypot(candidate.x - start.x, candidate.y - start.y);
                if (length > bestLength && segmentSupportedByInk(start, candidate)) {
                    bestLength = length;
                    end = candidate;
                }
            }
        }
        isolatedStrokes.push_back(Stroke{start, end});
    }

    std::vector<std::uint8_t> removedEdge(graphEdges.size(), 0);
    const auto directionFromNode = [&](const GraphEdge& edge, const std::size_t node) {
        PointF port = edge.left == node ? edge.points.front() : edge.points.back();
        PointF sample = port;
        const std::size_t sampleDistance = std::max<std::size_t>(
            1, static_cast<std::size_t>(std::lround(8.0F * processingScale)));
        if (edge.left == node) {
            sample = edge.points[std::min(edge.points.size() - 1, sampleDistance)];
        } else {
            sample = edge.points[edge.points.size() - 1 -
                                 std::min(edge.points.size() - 1, sampleDistance)];
        }
        const float x = sample.x - port.x;
        const float y = sample.y - port.y;
        const float length = std::hypot(x, y);
        return length > 1.0e-5F ? PointF{x / length, y / length} : PointF{};
    };

    // A terminal edge is removed only when geometry and the stage-two evidence map
    // both identify it as a medial-axis artifact. Strong original short lines remain
    // protected even when they are shorter than neighboring branches.
    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!nodes[node].junction || graphAdjacency[node].size() < 3) {
            continue;
        }
        const float maximumArtifactLength = std::clamp(
            localInkRadius(nodes[node].point) * 2.25F + processingScale,
            3.0F * processingScale,
            12.0F * processingScale);
        for (const std::size_t candidateIndex : graphAdjacency[node]) {
            if (removedEdge[candidateIndex] != 0) {
                continue;
            }
            const GraphEdge& candidate = graphEdges[candidateIndex];
            if (candidate.left == candidate.right ||
                candidate.length > maximumArtifactLength) {
                continue;
            }
            const std::size_t otherNode = candidate.left == node
                ? candidate.right
                : candidate.left;
            if (otherNode == node || nodes[otherNode].junction ||
                graphAdjacency[otherNode].size() != 1) {
                continue;
            }

            std::vector<std::size_t> others;
            std::vector<float> otherLengths;
            for (const std::size_t edgeIndex : graphAdjacency[node]) {
                if (edgeIndex != candidateIndex && removedEdge[edgeIndex] == 0) {
                    others.push_back(edgeIndex);
                    otherLengths.push_back(graphEdges[edgeIndex].length);
                }
            }
            if (others.size() < 2) {
                continue;
            }
            std::ranges::sort(otherLengths);
            const float medianLength = otherLengths[otherLengths.size() / 2];
            if (medianLength < std::max(maximumArtifactLength * 1.25F,
                                        candidate.length * 2.25F)) {
                continue;
            }

            const bool weakEvidence =
                (candidate.lowConfidenceRatio > 0.60F &&
                 candidate.endpointConfidence < 0.42F &&
                 candidate.multiScaleSupport < 0.24F &&
                 candidate.originalSupport < 0.24F &&
                 candidate.confidence < 0.42F) ||
                (candidate.repairedSupport > 0.60F &&
                 candidate.originalSupport < 0.18F);
            if (!weakEvidence) {
                continue;
            }

            const PointF shortDirection = directionFromNode(candidate, node);
            bool matchesFalseBisector = false;
            for (std::size_t left = 0; left < others.size(); ++left) {
                const PointF leftDirection = directionFromNode(graphEdges[others[left]], node);
                for (std::size_t right = left + 1; right < others.size(); ++right) {
                    const PointF rightDirection =
                        directionFromNode(graphEdges[others[right]], node);
                    const float pairCosine = leftDirection.x * rightDirection.x +
                                             leftDirection.y * rightDirection.y;
                    const float sumX = leftDirection.x + rightDirection.x;
                    const float sumY = leftDirection.y + rightDirection.y;
                    const float sumLength = std::hypot(sumX, sumY);
                    if (pairCosine <= -0.9F || sumLength <= 0.25F) {
                        continue;
                    }
                    const float oppositeBisector =
                        -(shortDirection.x * sumX + shortDirection.y * sumY) / sumLength;
                    if (oppositeBisector >= 0.72F) {
                        matchesFalseBisector = true;
                        break;
                    }
                }
                if (matchesFalseBisector) {
                    break;
                }
            }
            if (matchesFalseBisector) {
                removedEdge[candidateIndex] = 1;
            }
        }
    }

    if (debug != nullptr) {
        debug->removedSpurs = GrayImage{
            .width = width,
            .height = height,
            .pixels = std::vector<std::uint8_t>(skeleton.size(), 0),
        };
        debug->removedSpurCount = 0;
        for (std::size_t edgeIndex = 0; edgeIndex < graphEdges.size(); ++edgeIndex) {
            if (removedEdge[edgeIndex] == 0) {
                continue;
            }
            ++debug->removedSpurCount;
            for (const PointF point : graphEdges[edgeIndex].points) {
                debug->removedSpurs.pixels[pointPixel(point)] = 255;
            }
        }
    }

    struct JunctionPort {
        std::size_t edge{};
        PointF point;
        PointF outward;
        float localRadius{};
        float confidence{};
        float multiScaleSupport{};
        float originalSupport{};
        float repairedSupport{};
    };
    std::vector<Stroke> junctionConnectors;
    std::vector<std::size_t> predecessor(skeleton.size(), missing);
    std::vector<std::size_t> touchedPredecessors;
    const auto shortestNodePath = [&](const std::size_t node,
                                      const PointF from,
                                      const PointF to) {
        const std::size_t start = pointPixel(from);
        const std::size_t target = pointPixel(to);
        Stroke path;
        if (nodeByPixel[start] != node || nodeByPixel[target] != node) {
            return path;
        }
        std::queue<std::size_t> pending;
        predecessor[start] = start;
        touchedPredecessors.push_back(start);
        pending.push(start);
        while (!pending.empty() && predecessor[target] == missing) {
            const std::size_t current = pending.front();
            pending.pop();
            for (const std::size_t neighbor : pixelAdjacency[current]) {
                if (nodeByPixel[neighbor] == node && predecessor[neighbor] == missing) {
                    predecessor[neighbor] = current;
                    touchedPredecessors.push_back(neighbor);
                    pending.push(neighbor);
                }
            }
        }
        if (predecessor[target] != missing) {
            for (std::size_t current = target;; current = predecessor[current]) {
                path.push_back(PointF{
                    static_cast<float>(current % width),
                    static_cast<float>(current / width),
                });
                if (current == start) {
                    break;
                }
            }
            std::ranges::reverse(path);
        }
        for (const std::size_t pixel : touchedPredecessors) {
            predecessor[pixel] = missing;
        }
        touchedPredecessors.clear();
        return path;
    };
    const auto validRayIntersection = [&](const std::size_t node,
                                          const JunctionPort& left,
                                          const JunctionPort& right)
        -> std::optional<PointF> {
        const PointF leftInward{-left.outward.x, -left.outward.y};
        const PointF rightInward{-right.outward.x, -right.outward.y};
        const float cross = leftInward.x * rightInward.y -
                            leftInward.y * rightInward.x;
        if (std::abs(cross) < 0.08F) {
            return std::nullopt;
        }
        const PointF delta{
            right.point.x - left.point.x,
            right.point.y - left.point.y,
        };
        const float leftDistance =
            (delta.x * rightInward.y - delta.y * rightInward.x) / cross;
        const float rightDistance =
            (delta.x * leftInward.y - delta.y * leftInward.x) / cross;
        const float maximumTravel = std::max(
            3.0F, localInkRadius(nodes[node].point) * 3.0F + 2.0F * processingScale);
        if (leftDistance < -0.25F || rightDistance < -0.25F ||
            leftDistance > maximumTravel || rightDistance > maximumTravel) {
            if (debug != nullptr &&
                (leftDistance < -0.25F || rightDistance < -0.25F)) {
                ++debug->reverseRayRejectionCount;
            }
            return std::nullopt;
        }
        const PointF candidate{
            left.point.x + leftInward.x * leftDistance,
            left.point.y + leftInward.y * leftDistance,
        };
        float nearestNodePixel = std::numeric_limits<float>::infinity();
        for (const std::size_t pixel : nodes[node].pixels) {
            nearestNodePixel = std::min(
                nearestNodePixel,
                std::hypot(
                    static_cast<float>(pixel % width) - candidate.x,
                    static_cast<float>(pixel / width) - candidate.y));
        }
        if (nearestNodePixel > std::max(1.5F, processingScale) ||
            !supportedByInk(candidate) ||
            !segmentSupportedByInk(left.point, candidate) ||
            !segmentSupportedByInk(candidate, right.point)) {
            return std::nullopt;
        }
        return candidate;
    };

    for (std::size_t node = 0; node < nodes.size(); ++node) {
        if (!nodes[node].junction) {
            continue;
        }
        std::vector<JunctionPort> ports;
        for (const std::size_t edgeIndex : graphAdjacency[node]) {
            if (removedEdge[edgeIndex] != 0) {
                continue;
            }
            const GraphEdge& edge = graphEdges[edgeIndex];
            if (edge.left == edge.right || (edge.left != node && edge.right != node)) {
                continue;
            }
            ports.push_back(JunctionPort{
                .edge = edgeIndex,
                .point = edge.left == node ? edge.points.front() : edge.points.back(),
                .outward = directionFromNode(edge, node),
                .localRadius = localInkRadius(
                    edge.left == node ? edge.points.front() : edge.points.back()),
                .confidence = edge.confidence,
                .multiScaleSupport = edge.multiScaleSupport,
                .originalSupport = edge.originalSupport,
                .repairedSupport = edge.repairedSupport,
            });
        }
        if (ports.size() < 2) {
            continue;
        }
        if (debug != nullptr) {
            debug->junctionPortCount += ports.size();
        }

        const auto pairingCost = [&](const std::size_t left, const std::size_t right) {
            return 1.0F + ports[left].outward.x * ports[right].outward.x +
                   ports[left].outward.y * ports[right].outward.y;
        };
        if (ports.size() == 3) {
            std::size_t left = 0;
            std::size_t right = 1;
            float bestCost = pairingCost(left, right);
            for (std::size_t candidateLeft = 0; candidateLeft < ports.size(); ++candidateLeft) {
                for (std::size_t candidateRight = candidateLeft + 1;
                     candidateRight < ports.size();
                     ++candidateRight) {
                    const float cost = pairingCost(candidateLeft, candidateRight);
                    if (cost < bestCost) {
                        bestCost = cost;
                        left = candidateLeft;
                        right = candidateRight;
                    }
                }
            }
            const std::size_t branch = 3U - left - right;
            Stroke through = shortestNodePath(
                node, ports[left].point, ports[right].point);
            if (through.size() < 2) {
                through = Stroke{ports[left].point, ports[right].point};
            }
            const auto attachment = static_cast<std::size_t>(std::distance(
                through.begin(),
                std::ranges::min_element(
                    through,
                    {},
                    [&](const PointF point) {
                        return std::hypot(
                            point.x - ports[branch].point.x,
                            point.y - ports[branch].point.y);
                    })));
            if (attachment > 0) {
                junctionConnectors.emplace_back(
                    through.begin(), through.begin() + static_cast<std::ptrdiff_t>(attachment + 1));
            }
            if (attachment + 1 < through.size()) {
                junctionConnectors.emplace_back(
                    through.begin() + static_cast<std::ptrdiff_t>(attachment), through.end());
            }
            Stroke branchPath = shortestNodePath(
                node, ports[branch].point, through[attachment]);
            if (branchPath.size() >= 2) {
                junctionConnectors.push_back(std::move(branchPath));
                if (debug != nullptr) {
                    ++debug->geodesicConnectorCount;
                }
            }
            continue;
        }

        std::vector<std::uint8_t> paired(ports.size(), 0);
        while (std::ranges::count(paired, std::uint8_t{0}) >= 2) {
            std::size_t bestLeft = ports.size();
            std::size_t bestRight = ports.size();
            float bestCost = std::numeric_limits<float>::infinity();
            for (std::size_t left = 0; left < ports.size(); ++left) {
                if (paired[left] != 0) {
                    continue;
                }
                for (std::size_t right = left + 1; right < ports.size(); ++right) {
                    if (paired[right] != 0) {
                        continue;
                    }
                    const float cost = pairingCost(left, right);
                    if (cost < bestCost) {
                        bestCost = cost;
                        bestLeft = left;
                        bestRight = right;
                    }
                }
            }
            if (bestLeft == ports.size()) {
                break;
            }
            paired[bestLeft] = 1;
            paired[bestRight] = 1;
            if (const auto intersection = validRayIntersection(
                    node, ports[bestLeft], ports[bestRight])) {
                junctionConnectors.push_back(Stroke{
                    ports[bestLeft].point,
                    *intersection,
                    ports[bestRight].point,
                });
                if (debug != nullptr) {
                    ++debug->acceptedRayIntersectionCount;
                }
            } else {
                Stroke connector = shortestNodePath(
                    node, ports[bestLeft].point, ports[bestRight].point);
                if (connector.size() >= 2) {
                    junctionConnectors.push_back(std::move(connector));
                    if (debug != nullptr) {
                        ++debug->geodesicConnectorCount;
                    }
                }
            }
        }
        const auto unpaired = std::ranges::find(paired, std::uint8_t{0});
        if (unpaired != paired.end() && !junctionConnectors.empty()) {
            const std::size_t port = static_cast<std::size_t>(unpaired - paired.begin());
            PointF attachment = junctionConnectors.back().front();
            if (std::hypot(
                    junctionConnectors.back().back().x - ports[port].point.x,
                    junctionConnectors.back().back().y - ports[port].point.y) <
                std::hypot(
                    attachment.x - ports[port].point.x,
                    attachment.y - ports[port].point.y)) {
                attachment = junctionConnectors.back().back();
            }
            Stroke connector = shortestNodePath(node, ports[port].point, attachment);
            if (connector.size() >= 2) {
                junctionConnectors.push_back(std::move(connector));
                if (debug != nullptr) {
                    ++debug->geodesicConnectorCount;
                }
            }
        }
    }

    std::vector<Stroke> strokes;
    strokes.reserve(
        graphEdges.size() + junctionConnectors.size() + closedStrokes.size());
    for (std::size_t edgeIndex = 0; edgeIndex < graphEdges.size(); ++edgeIndex) {
        if (removedEdge[edgeIndex] != 0) {
            continue;
        }
        if (graphEdges[edgeIndex].points.size() >= 2) {
            strokes.push_back(simplifyWithinInk(graphEdges[edgeIndex].points));
        }
    }
    for (Stroke& connector : junctionConnectors) {
        if (connector.size() >= 2 && strokeSupportedByInk(connector)) {
            strokes.push_back(simplifyWithinInk(std::move(connector)));
        }
    }
    strokes.insert(
        strokes.end(),
        std::make_move_iterator(closedStrokes.begin()),
        std::make_move_iterator(closedStrokes.end()));
    strokes.insert(
        strokes.end(),
        std::make_move_iterator(isolatedStrokes.begin()),
        std::make_move_iterator(isolatedStrokes.end()));
    for (Stroke& stroke : strokes) {
        stroke = simplifyWithinInk(std::move(stroke));
    }

    // Thinning can occasionally erase an entire very small blob. Verify source
    // component coverage after graph extraction and add a source-constrained micro
    // stroke for any component that still has no route at all.
    std::vector<std::uint8_t> routeSupport(sourceInk.size(), 0);
    const auto markNearestSourceInk = [&](const PointF point) {
        const int centerX = static_cast<int>(std::lround(point.x));
        const int centerY = static_cast<int>(std::lround(point.y));
        float bestDistance = std::numeric_limits<float>::infinity();
        std::size_t best = sourceInk.size();
        for (int offsetY = -1; offsetY <= 1; ++offsetY) {
            for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                const int x = centerX + offsetX;
                const int y = centerY + offsetY;
                if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
                    y >= static_cast<int>(height)) {
                    continue;
                }
                const std::size_t index = static_cast<std::size_t>(y) * width +
                                          static_cast<std::size_t>(x);
                const float distance = std::hypot(
                    static_cast<float>(x) - point.x,
                    static_cast<float>(y) - point.y);
                if (sourceInk[index] != 0 && distance < bestDistance) {
                    bestDistance = distance;
                    best = index;
                }
            }
        }
        if (best != sourceInk.size()) {
            routeSupport[best] = 1;
        }
    };
    for (const Stroke& stroke : strokes) {
        for (std::size_t index = 1; index < stroke.size(); ++index) {
            const PointF start = stroke[index - 1];
            const PointF end = stroke[index];
            const int steps = std::max(
                1,
                static_cast<int>(std::ceil(
                    std::hypot(end.x - start.x, end.y - start.y) * 2.0F)));
            for (int step = 0; step <= steps; ++step) {
                const float ratio = static_cast<float>(step) / static_cast<float>(steps);
                markNearestSourceInk(PointF{
                    start.x + (end.x - start.x) * ratio,
                    start.y + (end.y - start.y) * ratio,
                });
            }
        }
    }

    std::vector<std::uint8_t> visitedSource(sourceInk.size(), 0);
    std::queue<std::size_t> pendingSource;
    std::vector<std::size_t> sourceComponent;
    for (std::size_t componentStart = 0;
         componentStart < sourceInk.size();
         ++componentStart) {
        if (sourceInk[componentStart] == 0 || visitedSource[componentStart] != 0) {
            continue;
        }
        sourceComponent.clear();
        bool represented = false;
        double sumX = 0.0;
        double sumY = 0.0;
        visitedSource[componentStart] = 1;
        pendingSource.push(componentStart);
        while (!pendingSource.empty()) {
            const std::size_t current = pendingSource.front();
            pendingSource.pop();
            sourceComponent.push_back(current);
            represented = represented || routeSupport[current] != 0;
            const int x = static_cast<int>(current % width);
            const int y = static_cast<int>(current / width);
            sumX += x;
            sumY += y;
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    const int neighborX = x + offsetX;
                    const int neighborY = y + offsetY;
                    if ((offsetX == 0 && offsetY == 0) || neighborX < 0 || neighborY < 0 ||
                        neighborX >= static_cast<int>(width) ||
                        neighborY >= static_cast<int>(height)) {
                        continue;
                    }
                    const std::size_t neighbor =
                        static_cast<std::size_t>(neighborY) * width +
                        static_cast<std::size_t>(neighborX);
                    if (sourceInk[neighbor] != 0 && visitedSource[neighbor] == 0) {
                        visitedSource[neighbor] = 1;
                        pendingSource.push(neighbor);
                    }
                }
            }
        }
        if (represented || sourceComponent.empty()) {
            continue;
        }

        const PointF centroid{
            static_cast<float>(sumX / static_cast<double>(sourceComponent.size())),
            static_cast<float>(sumY / static_cast<double>(sourceComponent.size())),
        };
        const std::size_t centerPixel = *std::ranges::min_element(
            sourceComponent,
            {},
            [&](const std::size_t pixel) {
                return std::hypot(
                    static_cast<float>(pixel % width) - centroid.x,
                    static_cast<float>(pixel / width) - centroid.y);
            });
        const PointF start{
            static_cast<float>(centerPixel % width),
            static_cast<float>(centerPixel / width),
        };
        PointF end = start;
        float bestLength = 0.0F;
        for (const std::size_t pixel : sourceComponent) {
            const PointF candidate{
                static_cast<float>(pixel % width),
                static_cast<float>(pixel / width),
            };
            const float length = std::hypot(candidate.x - start.x, candidate.y - start.y);
            if (length > bestLength && segmentSupportedByInk(start, candidate)) {
                bestLength = length;
                end = candidate;
            }
        }
        strokes.push_back(Stroke{start, end});
    }

    // Nearest-endpoint ordering only changes pen-up travel and never changes line geometry.
    std::vector<Stroke> ordered;
    ordered.reserve(strokes.size());
    PointF current{static_cast<float>(width) * 0.5F, static_cast<float>(height) * 0.5F};
    while (!strokes.empty()) {
        std::size_t bestIndex = 0;
        bool reverseBest = false;
        float bestDistance = std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < strokes.size(); ++index) {
            const float frontDistance = std::hypot(
                strokes[index].front().x - current.x, strokes[index].front().y - current.y);
            const float backDistance = std::hypot(
                strokes[index].back().x - current.x, strokes[index].back().y - current.y);
            if (std::min(frontDistance, backDistance) < bestDistance) {
                bestDistance = std::min(frontDistance, backDistance);
                bestIndex = index;
                reverseBest = backDistance < frontDistance;
            }
        }
        if (reverseBest) {
            std::ranges::reverse(strokes[bestIndex]);
        }
        current = strokes[bestIndex].back();
        ordered.push_back(std::move(strokes[bestIndex]));
        strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(bestIndex));
    }
    const auto pointPixelSafe = [&](const PointF point) {
        const int x = std::clamp(
            static_cast<int>(std::lround(point.x)), 0, static_cast<int>(width) - 1);
        const int y = std::clamp(
            static_cast<int>(std::lround(point.y)), 0, static_cast<int>(height) - 1);
        return static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x);
    };
    const auto pointRegion = [&](const PointF point) {
        const std::size_t pixel = pointPixelSafe(point);
        return pixel < regionTypes.size() && regionTypes[pixel] <=
                static_cast<std::uint8_t>(LineRegionType::Ambiguous)
            ? static_cast<LineRegionType>(regionTypes[pixel])
            : LineRegionType::Ambiguous;
    };
    const auto segmentRegion = [&](const PointF left, const PointF right) {
        const LineRegionType leftType = pointRegion(left);
        const LineRegionType rightType = pointRegion(right);
        if (leftType == rightType) {
            return leftType;
        }
        if (leftType == LineRegionType::Junction ||
            rightType == LineRegionType::Junction) {
            return LineRegionType::Junction;
        }
        return rightType;
    };

    std::vector<StrokeRouteMetadata> metadata;
    metadata.reserve(ordered.size());
    for (const Stroke& stroke : ordered) {
        StrokeRouteMetadata strokeMetadata{
            .spans = {},
            .closed = stroke.size() >= 3 && stroke.front() == stroke.back(),
        };
        if (stroke.size() >= 2) {
            std::size_t spanBegin = 0;
            LineRegionType currentType = segmentRegion(stroke[0], stroke[1]);
            for (std::size_t segment = 1; segment + 1 < stroke.size(); ++segment) {
                const LineRegionType nextType =
                    segmentRegion(stroke[segment], stroke[segment + 1]);
                if (nextType == currentType) {
                    continue;
                }
                strokeMetadata.spans.push_back(RouteSpan{
                    .pointBegin = spanBegin,
                    .pointEnd = segment,
                    .regionType = currentType,
                    .beginAnchorFlags = spanBegin == 0
                        ? static_cast<std::uint8_t>(AnchorGraphEndpoint)
                        : static_cast<std::uint8_t>(AnchorRegionBoundary),
                    .endAnchorFlags = AnchorRegionBoundary,
                });
                spanBegin = segment;
                currentType = nextType;
            }
            strokeMetadata.spans.push_back(RouteSpan{
                .pointBegin = spanBegin,
                .pointEnd = stroke.size() - 1,
                .regionType = currentType,
                .beginAnchorFlags = spanBegin == 0
                    ? static_cast<std::uint8_t>(AnchorGraphEndpoint)
                    : static_cast<std::uint8_t>(AnchorRegionBoundary),
                .endAnchorFlags = AnchorGraphEndpoint,
            });

            const auto junctionFlag = [&](const PointF point) {
                const std::size_t pixel = pointPixelSafe(point);
                return pixel < junctionZone.size() && junctionZone[pixel] != 0
                    ? static_cast<std::uint8_t>(AnchorJunctionPort)
                    : static_cast<std::uint8_t>(AnchorNone);
            };
            strokeMetadata.spans.front().beginAnchorFlags |= junctionFlag(stroke.front());
            strokeMetadata.spans.back().endAnchorFlags |= junctionFlag(stroke.back());
            if (strokeMetadata.closed) {
                strokeMetadata.spans.front().beginAnchorFlags |= AnchorClosedSeam;
                strokeMetadata.spans.back().endAnchorFlags |= AnchorClosedSeam;
            }
        }
        metadata.push_back(std::move(strokeMetadata));
    }
    return TracedRoute{
        .strokes = std::move(ordered),
        .metadata = std::move(metadata),
    };
}

} // namespace

std::expected<ProcessedImage, std::wstring> ProcessImage(
    const std::filesystem::path& sourcePath,
    const ImageProcessingOptions& options,
    ImageProcessingDebug* const debug)
{
    if (debug != nullptr) {
        *debug = {};
    }
    if (sourcePath.empty() || !std::filesystem::is_regular_file(sourcePath)) {
        return std::unexpected(L"图片文件不存在。");
    }

    auto decoded = DecodeAndResize(sourcePath, options);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    const auto grayscale = ToGrayscale(*decoded);
    const float processingScale = std::max(
        1.0F,
        static_cast<float>(std::max(decoded->width, decoded->height)) / 768.0F);

    // Route extraction starts from the exact quality-first line-art mask. Keeping a
    // second XDoG route mask caused valid contours and small internal details to vanish
    // even though they were visible in the line-art preview.
    auto canonicalLineArt = ExtractCanonicalLineArt(
        *decoded,
        grayscale,
        decoded->width,
        decoded->height,
        processingScale,
        debug);

    auto routeData = BuildRouteSkeleton(
        canonicalLineArt.topology.pixels,
        canonicalLineArt.evidence,
        decoded->width,
        decoded->height,
        processingScale);
    if (debug != nullptr) {
        const std::size_t pixelCount = canonicalLineArt.evidence.size();
        debug->confidence = GrayImage{
            .width = decoded->width,
            .height = decoded->height,
            .pixels = std::vector<std::uint8_t>(pixelCount, 0),
        };
        debug->tangent = debug->confidence;
        debug->scaleSupport = debug->confidence;
        debug->provenance = debug->confidence;
        debug->regionTypes = debug->confidence;
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const LinePixelEvidence& sample = canonicalLineArt.evidence[pixel];
            debug->confidence.pixels[pixel] = sample.confidence;
            debug->tangent.pixels[pixel] = sample.tangentBin < 16
                ? static_cast<std::uint8_t>(17U * sample.tangentBin)
                : 0;
            debug->scaleSupport.pixels[pixel] =
                static_cast<std::uint8_t>(36U * std::min<std::uint8_t>(sample.scaleMask, 7));
            debug->provenance.pixels[pixel] =
                static_cast<std::uint8_t>(17U * std::min<std::uint8_t>(sample.flags, 15));
            if (pixel < routeData.regionTypes.size()) {
                debug->regionTypes.pixels[pixel] = static_cast<std::uint8_t>(
                    63U * std::min<std::uint8_t>(routeData.regionTypes[pixel], 4));
            }
        }
        debug->compactRegionCount = routeData.compactRegionCount;
        debug->elongatedRegionCount = routeData.elongatedRegionCount;
    }
    auto tracedRoute = TraceSkeleton(
        routeData.skeleton,
        decoded->width,
        decoded->height,
        canonicalLineArt.topology.pixels,
        canonicalLineArt.evidence,
        routeData.regionTypes,
        routeData.inkDistance,
        processingScale,
        debug);
    if (tracedRoute.strokes.empty()) {
        return std::unexpected(L"没有从图片中提取到可绘制线稿。");
    }

    DrawingPath drawingPath{
        .width = decoded->width,
        .height = decoded->height,
        .strokes = std::move(tracedRoute.strokes),
        .sourceEdgeEnds = {},
        .optimization = {},
        .routeMetadata = std::move(tracedRoute.metadata),
    };
    if (debug != nullptr) {
        debug->stage2UnoptimizedRoute = drawingPath;
    }
    if (options.optimizeDrawingTime) {
        drawingPath = OptimizeDrawingPathLossless(std::move(drawingPath));
    }
    std::optional<VectorDrawingPath> vectorPath;
    if (options.generateVectorPath) {
        vectorPath = FitVectorDrawingPath(drawingPath, canonicalLineArt.topology);
    }
    return ProcessedImage{
        .originalWidth = decoded->originalWidth,
        .originalHeight = decoded->originalHeight,
        .width = decoded->width,
        .height = decoded->height,
        .bgra = std::move(decoded->bgra),
        .lineArt = LineArtDocument{
            .coverageLineArt = std::move(canonicalLineArt.coverage),
            .cleanLineArt = std::move(canonicalLineArt.topology),
            .strokes = std::move(drawingPath),
            .vectorPath = std::move(vectorPath),
        },
        .sourcePath = sourcePath,
    };
}

} // namespace vrcdraw
