#include "ExecutionPlan.hpp"
#include "ImageProcessor.hpp"
#include "LineArtExporter.hpp"
#include "LineArtRenderer.hpp"
#include "PathMath.hpp"
#include "Settings.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {

#pragma pack(push, 1)
struct BitmapFileHeader {
    std::uint16_t type{0x4D42};
    std::uint32_t size{};
    std::uint16_t reserved1{};
    std::uint16_t reserved2{};
    std::uint32_t pixelOffset{54};
};

struct BitmapInfoHeader {
    std::uint32_t size{40};
    std::int32_t width{};
    std::int32_t height{};
    std::uint16_t planes{1};
    std::uint16_t bitCount{24};
    std::uint32_t compression{};
    std::uint32_t imageSize{};
    std::int32_t xPixelsPerMeter{2835};
    std::int32_t yPixelsPerMeter{2835};
    std::uint32_t colorsUsed{};
    std::uint32_t colorsImportant{};
};
#pragma pack(pop)

std::filesystem::path WriteTestBitmap()
{
    constexpr int width = 64;
    constexpr int height = 64;
    constexpr int stride = ((width * 3 + 3) / 4) * 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height, 255);

    const auto setBlack = [&](const int x, const int y) {
        const std::size_t offset = static_cast<std::size_t>(height - 1 - y) * stride +
                                   static_cast<std::size_t>(x) * 3;
        pixels[offset] = 0;
        pixels[offset + 1] = 0;
        pixels[offset + 2] = 0;
    };
    for (int x = 10; x <= 53; ++x) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            setBlack(x, 10 + thickness);
            setBlack(x, 53 + thickness);
        }
    }
    for (int y = 10; y <= 53; ++y) {
        for (int thickness = -1; thickness <= 1; ++thickness) {
            setBlack(10 + thickness, y);
            setBlack(53 + thickness, y);
        }
    }

    BitmapFileHeader fileHeader{};
    BitmapInfoHeader infoHeader{};
    infoHeader.width = width;
    infoHeader.height = height;
    infoHeader.imageSize = static_cast<std::uint32_t>(pixels.size());
    fileHeader.size = fileHeader.pixelOffset + infoHeader.imageSize;

    const auto path = std::filesystem::current_path() / L"vrcdraw-core-test.bmp";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    output.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return path;
}

void TestPathSimplification()
{
    const vrcdraw::Stroke straight{{0, 0}, {1, 0}, {2, 0}, {3, 0}};
    const auto simplifiedStraight = vrcdraw::SimplifyRdp(straight, 0.1F);
    assert(simplifiedStraight.size() == 2);

    const vrcdraw::Stroke corner{{0, 0}, {1, 0}, {1, 1}, {2, 1}};
    const auto simplifiedCorner = vrcdraw::SimplifyRdp(corner, 0.1F);
    assert(simplifiedCorner.size() >= 3);
}

void TestMouseInterpolation()
{
    const vrcdraw::MousePoint start{0, 0};
    const vrcdraw::MousePoint target{12, 3};
    const auto deltas = vrcdraw::InterpolateMouseLine(start, target, 3);
    assert(!deltas.empty());

    vrcdraw::MousePoint current = start;
    for (const auto delta : deltas) {
        assert(std::abs(delta.dx) <= 3);
        assert(std::abs(delta.dy) <= 3);
        current.x += delta.dx;
        current.y += delta.dy;
        const float distance = vrcdraw::PointSegmentDistance(
            vrcdraw::PointF{static_cast<float>(current.x), static_cast<float>(current.y)},
            vrcdraw::PointF{0, 0},
            vrcdraw::PointF{12, 3});
        assert(distance <= 1.0F);
    }
    assert(current == target);

    const auto negative = vrcdraw::InterpolateMouseLine({4, 8}, {-3, -17}, 2);
    current = {4, 8};
    for (const auto delta : negative) {
        assert(std::abs(delta.dx) <= 2);
        assert(std::abs(delta.dy) <= 2);
        current.x += delta.dx;
        current.y += delta.dy;
    }
    assert(current == vrcdraw::MousePoint(-3, -17));
}

void TestExecutionPlan()
{
    vrcdraw::DrawingPath path{
        .width = 32,
        .height = 32,
        .strokes = {{{4, 8}, {16, 11}, {27, 25}}},
    };
    const auto plan = vrcdraw::BuildExecutionPlan(path);
    assert(plan.strokeCount == 1);
    assert(!plan.commands.empty());
    assert(plan.estimatedDuration.count() > 0);

    bool penDown = false;
    bool sawPenDownMove = false;
    bool sawTravelMove = false;
    std::size_t downCount = 0;
    std::size_t upCount = 0;
    vrcdraw::MousePoint current{};
    for (const auto& command : plan.commands) {
        switch (command.type) {
        case vrcdraw::MouseCommandType::LeftDown:
            penDown = true;
            ++downCount;
            break;
        case vrcdraw::MouseCommandType::LeftUp:
            penDown = false;
            ++upCount;
            break;
        case vrcdraw::MouseCommandType::Move:
            assert(std::abs(command.dx) <= 2);
            assert(std::abs(command.dy) <= 2);
            sawTravelMove = sawTravelMove || !penDown;
            current.x += command.dx;
            current.y += command.dy;
            sawPenDownMove = sawPenDownMove || penDown;
            break;
        case vrcdraw::MouseCommandType::Wait:
            break;
        }
    }
    assert(!penDown);
    assert(sawPenDownMove);
    assert(sawTravelMove);
    assert(downCount == 1);
    assert(upCount == 1);
    assert(current == vrcdraw::MousePoint(11, 9));

    for (std::size_t index = 0; index < plan.commands.size(); ++index) {
        if (plan.commands[index].type == vrcdraw::MouseCommandType::Move) {
            assert(index + 1 < plan.commands.size());
            assert(plan.commands[index + 1].type == vrcdraw::MouseCommandType::Wait);
            assert(plan.commands[index + 1].duration == std::chrono::milliseconds(16));
        }
        if (plan.commands[index].type == vrcdraw::MouseCommandType::LeftUp) {
            assert(index + 1 < plan.commands.size());
            assert(plan.commands[index + 1].type == vrcdraw::MouseCommandType::Wait);
            assert(plan.commands[index + 1].duration == std::chrono::milliseconds(32));
        }
    }

    const vrcdraw::DrawingPath shortStrokes{
        .width = 32,
        .height = 32,
        .strokes = {
            {{15, 15}, {16, 15}},
            {{16, 16}, {17, 16}},
            {{17, 17}, {18, 17}},
        },
    };
    const vrcdraw::ExecutionOptions options{};
    const auto guardedPlan = vrcdraw::BuildExecutionPlan(shortStrokes, options);
    bool hasPreviousDown = false;
    std::chrono::milliseconds elapsed{};
    std::chrono::milliseconds previousDownAt{};
    for (const auto& command : guardedPlan.commands) {
        if (command.type == vrcdraw::MouseCommandType::Wait) {
            elapsed += command.duration;
        } else if (command.type == vrcdraw::MouseCommandType::LeftDown) {
            if (hasPreviousDown) {
                assert(elapsed - previousDownAt >= options.minimumButtonDownInterval);
            }
            previousDownAt = elapsed;
            hasPreviousDown = true;
        }
    }
    assert(guardedPlan.strokeCount == 3);
}

void TestSettings(const std::filesystem::path& directory)
{
    const auto path = directory / L"settings-test.ini";
    const vrcdraw::Settings expected{
        .futureStrokeLimit = 777,
        .drawingHotkeyVirtualKey = 0x79,
        .drawingHotkeyModifiers = 0x02,
    };
    assert(vrcdraw::SaveSettings(path, expected));
    const auto actual = vrcdraw::LoadSettings(path);
    assert(actual.futureStrokeLimit == expected.futureStrokeLimit);
    assert(actual.drawingHotkeyVirtualKey == expected.drawingHotkeyVirtualKey);
    assert(actual.drawingHotkeyModifiers == expected.drawingHotkeyModifiers);
    std::error_code error;
    std::filesystem::remove(path, error);
}

void TestProgramIcon()
{
    const auto path =
        std::filesystem::path(VRC_DRAW_SOURCE_DIR) / L"resources" / L"VRC-Draw.ico";
    std::ifstream icon(path, std::ios::binary);
    assert(icon.good());

    std::array<unsigned char, 6> header{};
    icon.read(reinterpret_cast<char*>(header.data()), header.size());
    assert(icon.gcount() == static_cast<std::streamsize>(header.size()));
    assert(header[0] == 0 && header[1] == 0);
    assert(header[2] == 1 && header[3] == 0);
    const unsigned int imageCount = header[4] | (static_cast<unsigned int>(header[5]) << 8U);
    assert(imageCount == 9);

    std::array<unsigned int, 9> actualSizes{};
    for (unsigned int index = 0; index < imageCount; ++index) {
        std::array<unsigned char, 16> entry{};
        icon.read(reinterpret_cast<char*>(entry.data()), entry.size());
        assert(icon.gcount() == static_cast<std::streamsize>(entry.size()));
        actualSizes[index] = entry[0] == 0 ? 256U : entry[0];
        const unsigned int height = entry[1] == 0 ? 256U : entry[1];
        assert(actualSizes[index] == height);
    }
    constexpr std::array<unsigned int, 9> expectedSizes{
        16, 20, 24, 32, 40, 48, 64, 128, 256};
    assert(actualSizes == expectedSizes);

    for (const unsigned int size : expectedSizes) {
        const auto individualPath = std::filesystem::path(VRC_DRAW_SOURCE_DIR) /
                                    L"resources" / L"icons" /
                                    (L"VRC-Draw-" + std::to_wstring(size) + L".ico");
        std::ifstream individual(individualPath, std::ios::binary);
        assert(individual.good());
        std::array<unsigned char, 22> data{};
        individual.read(reinterpret_cast<char*>(data.data()), data.size());
        assert(individual.gcount() == static_cast<std::streamsize>(data.size()));
        assert(data[4] == 1 && data[5] == 0);
        const unsigned int width = data[6] == 0 ? 256U : data[6];
        const unsigned int height = data[7] == 0 ? 256U : data[7];
        assert(width == size && height == size);
    }
}

void TestImageProcessing()
{
    const auto path = WriteTestBitmap();
    const auto result = vrcdraw::ProcessImage(path);
    if (!result) {
        std::wcerr << L"Image processing failed: " << result.error() << L'\n';
    }
    assert(result.has_value());
    assert(result->width == 64);
    assert(result->height == 64);
    assert(!result->lineArt.strokes.strokes.empty());
    assert(result->lineArt.cleanLineArt.width == 64);
    assert(result->lineArt.cleanLineArt.height == 64);
    assert(result->lineArt.cleanLineArt.pixels.size() == 64U * 64U);

    // The line-art document preserves source ink width; skeletonization belongs only to routes.
    std::size_t horizontalLinePixels = 0;
    for (std::size_t y = 5; y <= 15; ++y) {
        horizontalLinePixels +=
            result->lineArt.cleanLineArt.pixels[y * 64U + 32U] != 0;
    }
    std::size_t verticalLinePixels = 0;
    for (std::size_t x = 5; x <= 15; ++x) {
        verticalLinePixels +=
            result->lineArt.cleanLineArt.pixels[32U * 64U + x] != 0;
    }
    assert(horizontalLinePixels >= 3);
    assert(verticalLinePixels >= 3);

    const auto rendered = vrcdraw::RenderBinaryLineArt(result->lineArt.cleanLineArt);
    assert(rendered.bgra.size() == 64U * 64U * 4U);
    const bool hasDarkPixel = std::ranges::any_of(rendered.bgra, [](const std::uint8_t value) {
        return value < 128;
    });
    assert(hasDarkPixel);

    const auto routeRendered = vrcdraw::RenderLineArt(result->lineArt.strokes, 64, 64);
    std::size_t lineArtInk = 0;
    std::size_t routeInk = 0;
    for (std::size_t pixel = 0; pixel < 64U * 64U; ++pixel) {
        lineArtInk += rendered.bgra[pixel * 4U] < 128;
        routeInk += routeRendered.bgra[pixel * 4U] < 128;
    }
    assert(lineArtInk > routeInk);

    const auto exportDirectory = std::filesystem::current_path() / L"vrcdraw-export-test";
    std::error_code error;
    std::filesystem::remove_all(exportDirectory, error);
    assert(!error);
    const auto exported = vrcdraw::ExportLineArt(*result, exportDirectory);
    if (!exported) {
        std::wcerr << L"Line-art export failed: " << exported.error() << L'\n';
    }
    assert(exported.has_value());
    assert(std::filesystem::file_size(exported->pngPath) > 8);
    assert(std::filesystem::file_size(exported->svgPath) > 32);
    std::string svgText;
    {
        std::ifstream svgInput(exported->svgPath, std::ios::binary);
        svgText.assign(
            std::istreambuf_iterator<char>(svgInput), std::istreambuf_iterator<char>{});
    }
    assert(svgText.contains("<rect x=\""));
    const auto exportedAgain = vrcdraw::ExportLineArt(*result, exportDirectory);
    assert(exportedAgain.has_value());
    assert(exportedAgain->pngPath.stem() == exportedAgain->svgPath.stem());
    assert(exportedAgain->pngPath != exported->pngPath);
    error.clear();
    std::filesystem::remove_all(exportDirectory, error);
    assert(!error);
    assert(!std::filesystem::exists(exportDirectory));
    std::filesystem::remove(path, error);
}

void TestRepositorySamples()
{
    const auto samples = std::filesystem::path(VRC_DRAW_SOURCE_DIR) / L"simple";
    if (!std::filesystem::is_directory(samples)) {
        return;
    }

    std::size_t tested = 0;
    for (const auto& entry : std::filesystem::directory_iterator(samples)) {
        if (!entry.is_regular_file() || entry.path().extension() != L".png") {
            continue;
        }
        const auto result = vrcdraw::ProcessImage(entry.path());
        if (!result) {
            std::wcerr << L"Sample processing failed for " << entry.path() << L": "
                       << result.error() << L'\n';
        }
        assert(result.has_value());
        assert(!result->lineArt.strokes.strokes.empty());
        const auto plan = vrcdraw::BuildExecutionPlan(result->lineArt.strokes);
        assert(plan.strokeCount == result->lineArt.strokes.strokes.size());
        assert(!plan.commands.empty());
        assert(plan.estimatedDuration < std::chrono::minutes(3));
        std::cout << "Sample strokes: " << result->lineArt.strokes.strokes.size()
                  << ", commands: " << plan.commands.size()
                  << ", duration_ms: " << plan.estimatedDuration.count() << '\n';
        ++tested;
    }
    assert(tested > 0);
}

} // namespace

int main()
{
    TestPathSimplification();
    TestMouseInterpolation();
    TestExecutionPlan();
    TestSettings(std::filesystem::current_path());
    TestProgramIcon();
    TestImageProcessing();
    TestRepositorySamples();
    std::cout << "All VRC-Draw core tests passed.\n";
    return 0;
}
