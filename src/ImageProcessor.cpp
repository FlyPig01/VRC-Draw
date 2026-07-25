#include "ImageProcessor.hpp"

#include "PathMath.hpp"

#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <numeric>
#include <queue>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vrcdraw {
namespace {

constexpr std::uint32_t kWorkingMaximumDimension = 768;
constexpr float kPathSimplificationEpsilon = 1.1F;
constexpr float kMinimumPathLength = 1.0F;
constexpr int kMaximumSkeletonIterations = 192;

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
    const std::filesystem::path& sourcePath)
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

    const double scale = std::min(
        1.0,
        static_cast<double>(kWorkingMaximumDimension) /
            static_cast<double>(std::max(sourceWidth, sourceHeight)));
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

std::vector<std::uint8_t> ToGrayscale(const DecodedImage& image)
{
    std::vector<std::uint8_t> grayscale(
        static_cast<std::size_t>(image.width) * image.height);
    for (std::size_t pixel = 0; pixel < grayscale.size(); ++pixel) {
        const std::size_t offset = pixel * 4;
        const std::uint32_t alpha = image.bgra[offset + 3];
        const std::uint32_t blue =
            (image.bgra[offset] * alpha + 255U * (255U - alpha)) / 255U;
        const std::uint32_t green =
            (image.bgra[offset + 1] * alpha + 255U * (255U - alpha)) / 255U;
        const std::uint32_t red =
            (image.bgra[offset + 2] * alpha + 255U * (255U - alpha)) / 255U;
        grayscale[pixel] = static_cast<std::uint8_t>(
            (77U * red + 150U * green + 29U * blue) >> 8U);
    }
    return grayscale;
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

std::vector<std::uint8_t> ExtractInkMask(
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
        return mask;
    }

    // XDoG-style dark-ridge response. Unlike a gradient edge detector this marks the
    // dark stroke itself, so skeletonization produces one centre line rather than two edges.
    const auto fine = GaussianBlur(grayscale, width, height, 0.8F);
    const auto coarse = GaussianBlur(grayscale, width, height, 1.6F);
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
    return mask;
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

std::vector<std::uint8_t> Erode3x3(
    const std::vector<std::uint8_t>& input,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::uint8_t> output(input.size(), 0);
    if (width < 3 || height < 3) {
        return output;
    }
    for (std::uint32_t y = 1; y + 1 < height; ++y) {
        for (std::uint32_t x = 1; x + 1 < width; ++x) {
            bool foreground = true;
            for (int offsetY = -1; offsetY <= 1 && foreground; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (input[static_cast<std::size_t>(static_cast<int>(y) + offsetY) * width +
                              static_cast<std::size_t>(static_cast<int>(x) + offsetX)] == 0) {
                        foreground = false;
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
    const std::uint32_t height)
{
    const std::size_t minimumSize = std::max<std::size_t>(8, mask.size() / 180000U);
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

void PruneShortSpurs(
    std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height)
{
    constexpr std::size_t maximumSpurLength = 2;
    for (int pass = 0; pass < 2; ++pass) {
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

Stroke SmoothStroke(const Stroke& stroke)
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

std::vector<Stroke> TraceSkeleton(
    const std::vector<std::uint8_t>& skeleton,
    const std::uint32_t width,
    const std::uint32_t height)
{
    std::vector<std::vector<std::size_t>> adjacency(skeleton.size());
    std::size_t edgeCount = 0;
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        if (skeleton[index] == 0) {
            continue;
        }
        adjacency[index] = PixelNeighbors(skeleton, width, height, index);
        edgeCount += adjacency[index].size();
    }

    std::unordered_set<std::uint64_t> visitedEdges;
    visitedEdges.reserve(edgeCount / 2 + 1);
    std::vector<Stroke> strokes;

    const auto hasUnvisitedEdge = [&](const std::size_t node) {
        return std::ranges::any_of(adjacency[node], [&](const std::size_t neighbor) {
            return !visitedEdges.contains(EdgeKey(node, neighbor));
        });
    };

    const auto traceFrom = [&](const std::size_t start) {
        Stroke raw;
        std::size_t previous = std::numeric_limits<std::size_t>::max();
        std::size_t current = start;
        for (;;) {
            raw.push_back(PointF{
                static_cast<float>(current % width),
                static_cast<float>(current / width),
            });

            std::size_t best = std::numeric_limits<std::size_t>::max();
            float bestScore = -std::numeric_limits<float>::infinity();
            const int currentX = static_cast<int>(current % width);
            const int currentY = static_cast<int>(current / width);
            for (const std::size_t candidate : adjacency[current]) {
                if (visitedEdges.contains(EdgeKey(current, candidate))) {
                    continue;
                }
                float score = 0.0F;
                if (previous != std::numeric_limits<std::size_t>::max()) {
                    const int previousX = static_cast<int>(previous % width);
                    const int previousY = static_cast<int>(previous / width);
                    const int candidateX = static_cast<int>(candidate % width);
                    const int candidateY = static_cast<int>(candidate / width);
                    const float incomingX = static_cast<float>(currentX - previousX);
                    const float incomingY = static_cast<float>(currentY - previousY);
                    const float outgoingX = static_cast<float>(candidateX - currentX);
                    const float outgoingY = static_cast<float>(candidateY - currentY);
                    score = (incomingX * outgoingX + incomingY * outgoingY) /
                            std::sqrt((incomingX * incomingX + incomingY * incomingY) *
                                      (outgoingX * outgoingX + outgoingY * outgoingY));
                } else {
                    score = -static_cast<float>(adjacency[candidate].size()) * 0.001F;
                }
                if (score > bestScore) {
                    bestScore = score;
                    best = candidate;
                }
            }
            if (best == std::numeric_limits<std::size_t>::max()) {
                break;
            }
            visitedEdges.insert(EdgeKey(current, best));
            previous = current;
            current = best;
        }
        return raw;
    };

    const auto appendTrace = [&](const std::size_t start) {
        Stroke raw = traceFrom(start);
        if (raw.size() < 2 || StrokeLength(raw) < kMinimumPathLength) {
            return;
        }
        Stroke simplified = SimplifyRdp(SmoothStroke(raw), kPathSimplificationEpsilon);
        if (simplified.size() >= 2) {
            strokes.push_back(std::move(simplified));
        }
    };

    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        if (skeleton[index] != 0 && adjacency[index].size() == 1 && hasUnvisitedEdge(index)) {
            appendTrace(index);
        }
    }
    for (std::size_t index = 0; index < skeleton.size(); ++index) {
        while (skeleton[index] != 0 && hasUnvisitedEdge(index)) {
            appendTrace(index);
        }
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
    return ordered;
}

} // namespace

std::expected<ProcessedImage, std::wstring> ProcessImage(
    const std::filesystem::path& sourcePath)
{
    if (sourcePath.empty() || !std::filesystem::is_regular_file(sourcePath)) {
        return std::unexpected(L"图片文件不存在。");
    }

    auto decoded = DecodeAndResize(sourcePath);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }

    const auto grayscale = ToGrayscale(*decoded);
    auto inkMask = ExtractInkMask(grayscale, decoded->width, decoded->height);
    inkMask = Erode3x3(Dilate3x3(inkMask, decoded->width, decoded->height),
                       decoded->width,
                       decoded->height);
    RemoveSmallComponents(inkMask, decoded->width, decoded->height);

    // The line-art page preserves the cleaned ink, including its original line width.
    // Skeletonization below is exclusively for the mouse route.
    BinaryImage cleanLineArt{
        .width = decoded->width,
        .height = decoded->height,
        .pixels = inkMask,
    };

    ThinZhangSuen(inkMask, decoded->width, decoded->height);
    PruneShortSpurs(inkMask, decoded->width, decoded->height);
    RemoveSmallComponents(inkMask, decoded->width, decoded->height);

    auto strokes = TraceSkeleton(inkMask, decoded->width, decoded->height);
    if (strokes.empty()) {
        return std::unexpected(L"没有从图片中提取到可绘制线稿。");
    }

    DrawingPath drawingPath{
        .width = decoded->width,
        .height = decoded->height,
        .strokes = std::move(strokes),
    };
    return ProcessedImage{
        .originalWidth = decoded->originalWidth,
        .originalHeight = decoded->originalHeight,
        .width = decoded->width,
        .height = decoded->height,
        .bgra = std::move(decoded->bgra),
        .lineArt = LineArtDocument{
            .cleanLineArt = std::move(cleanLineArt),
            .strokes = std::move(drawingPath),
        },
        .sourcePath = sourcePath,
    };
}

} // namespace vrcdraw
