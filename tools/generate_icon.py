#!/usr/bin/env python3
"""Render the repository SVG source into a multi-resolution Windows ICO.

This intentionally supports only the small SVG subset used by
assets/vrc-draw.svg. Pillow is a development-only dependency; end users and
normal C++ builds consume the committed ICO and do not need Python or Pillow.
"""

from __future__ import annotations

import argparse
import io
import struct
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageColor, ImageDraw


ICON_SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
SUPPORTED_ELEMENTS = {"svg", "title", "desc", "rect", "circle", "polygon", "polyline"}


def local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def parse_points(value: str) -> list[tuple[float, float]]:
    return [tuple(map(float, pair.split(","))) for pair in value.split()]


def scaled_points(
    points: list[tuple[float, float]], scale_x: float, scale_y: float
) -> list[tuple[int, int]]:
    return [(round(x * scale_x), round(y * scale_y)) for x, y in points]


def render_svg(svg_path: Path, size: int) -> Image.Image:
    root = ET.parse(svg_path).getroot()
    unknown = {local_name(node.tag) for node in root.iter()} - SUPPORTED_ELEMENTS
    if unknown:
        raise ValueError(f"Unsupported SVG elements: {', '.join(sorted(unknown))}")

    view_box = [float(value) for value in root.attrib["viewBox"].split()]
    if len(view_box) != 4 or view_box[0] != 0 or view_box[1] != 0:
        raise ValueError("The icon generator expects a zero-origin SVG viewBox")

    supersampling = 4
    render_size = size * supersampling
    scale_x = render_size / view_box[2]
    scale_y = render_size / view_box[3]
    image = Image.new("RGBA", (render_size, render_size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    for node in root:
        kind = local_name(node.tag)
        attributes = node.attrib
        if kind == "rect":
            x = round(float(attributes["x"]) * scale_x)
            y = round(float(attributes["y"]) * scale_y)
            width = round(float(attributes["width"]) * scale_x)
            height = round(float(attributes["height"]) * scale_y)
            radius = round(float(attributes.get("rx", "0")) * min(scale_x, scale_y))
            draw.rounded_rectangle(
                (x, y, x + width, y + height),
                radius=radius,
                fill=ImageColor.getcolor(attributes["fill"], "RGBA"),
            )
        elif kind == "circle":
            center_x = float(attributes["cx"]) * scale_x
            center_y = float(attributes["cy"]) * scale_y
            radius = float(attributes["r"]) * min(scale_x, scale_y)
            draw.ellipse(
                (
                    round(center_x - radius),
                    round(center_y - radius),
                    round(center_x + radius),
                    round(center_y + radius),
                ),
                fill=ImageColor.getcolor(attributes["fill"], "RGBA"),
            )
        elif kind == "polygon":
            draw.polygon(
                scaled_points(parse_points(attributes["points"]), scale_x, scale_y),
                fill=ImageColor.getcolor(attributes["fill"], "RGBA"),
            )
        elif kind == "polyline":
            points = scaled_points(parse_points(attributes["points"]), scale_x, scale_y)
            color = ImageColor.getcolor(attributes["stroke"], "RGBA")
            width = max(1, round(float(attributes["stroke-width"]) * min(scale_x, scale_y)))
            draw.line(points, fill=color, width=width, joint="curve")
            if attributes.get("stroke-linecap") == "round":
                radius = width // 2
                for x, y in (points[0], points[-1]):
                    draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)

    return image.resize((size, size), Image.Resampling.LANCZOS)


def encode_png(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(output, format="PNG", optimize=True)
    return output.getvalue()


def write_ico(path: Path, frames: list[tuple[int, bytes]]) -> None:
    directory_size = 6 + len(frames) * 16
    offset = directory_size
    entries: list[bytes] = []
    payloads: list[bytes] = []
    for size, payload in frames:
        dimension = 0 if size == 256 else size
        entries.append(
            struct.pack(
                "<BBBBHHII",
                dimension,
                dimension,
                0,
                0,
                1,
                32,
                len(payload),
                offset,
            )
        )
        payloads.append(payload)
        offset += len(payload)

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        struct.pack("<HHH", 0, 1, len(frames)) + b"".join(entries) + b"".join(payloads)
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("svg", type=Path)
    parser.add_argument("ico", type=Path)
    parser.add_argument(
        "--individual-dir",
        type=Path,
        help="Also write one single-resolution ICO for each generated size",
    )
    arguments = parser.parse_args()

    frames = [(size, encode_png(render_svg(arguments.svg, size))) for size in ICON_SIZES]
    write_ico(arguments.ico, frames)
    print(f"Generated {arguments.ico} with {len(frames)} sizes: {ICON_SIZES}")
    if arguments.individual_dir is not None:
        arguments.individual_dir.mkdir(parents=True, exist_ok=True)
        for size, payload in frames:
            output = arguments.individual_dir / f"VRC-Draw-{size}.ico"
            write_ico(output, [(size, payload)])
        print(
            f"Generated {len(frames)} single-resolution ICO files in "
            f"{arguments.individual_dir}"
        )


if __name__ == "__main__":
    main()
