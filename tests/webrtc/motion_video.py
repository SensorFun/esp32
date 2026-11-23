"""Render the firmware's moving-cell grid over the original replay frames."""
from io import BytesIO
from pathlib import Path
import shutil
import subprocess
import tempfile


def video_dependencies():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as error:
        raise ValueError("Video export requires Pillow. See the WebRTC test README.") from error
    encoder = shutil.which("ffmpeg")
    if not encoder:
        try:
            from imageio_ffmpeg import get_ffmpeg_exe
            encoder = get_ffmpeg_exe()
        except (ImportError, RuntimeError) as error:
            raise ValueError("Video export requires FFmpeg or imageio-ffmpeg.") from error
    return encoder, Image, ImageDraw, ImageFont


def motion_boxes(sample, width, height):
    if not sample or not sample["motion"]:
        return []
    cols, rows = sample["cols"], sample["rows"]
    active = {divmod(index, cols) for index, flag in enumerate(sample["grid"]) if flag == "1"}
    boxes = []
    while active:
        first = min(active)
        active.remove(first)
        pending = [first]
        top = bottom = first[0]
        left = right = first[1]
        while pending:
            y, x = pending.pop()
            top, bottom = min(y, top), max(y, bottom)
            left, right = min(x, left), max(x, right)
            for adjacent in ((y - 1, x), (y + 1, x), (y, x - 1), (y, x + 1)):
                if adjacent in active:
                    active.remove(adjacent)
                    pending.append(adjacent)
        # Match the detector's integer grid edges before projecting to the video.
        aw, ah = sample["width"], sample["height"]
        x0, x1 = left * aw // cols, (right + 1) * aw // cols
        y0, y1 = top * ah // rows, (bottom + 1) * ah // rows
        boxes.append((x0 * width // aw, y0 * height // ah,
                      (x1 * width + aw - 1) // aw - 1, (y1 * height + ah - 1) // ah - 1))
    return boxes


def export_video(path, frames, frame_us, trace, dependencies):
    encoder, Image, ImageDraw, ImageFont = dependencies
    with Image.open(BytesIO(frames[0][1])) as first:
        width, height = first.size
    font = ImageFont.load_default(size=max(13, width // 55))
    stroke = max(2, width // 320)
    samples = {row["capture_us"]: row for row in trace}
    sample = None
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=path.parent) as temp:
        target = Path(temp) / "video.mp4"
        command = [encoder, "-hide_banner", "-loglevel", "error", "-nostdin",
                   "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size",
                   f"{width}x{height}", "-framerate", f"1000000/{frame_us}", "-i", "pipe:0",
                   "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", "-filter_threads", "1",
                   "-c:v", "libx264", "-threads", "1", "-pix_fmt", "yuv420p",
                   "-movflags", "+faststart", str(target)]
        with subprocess.Popen(command, stdin=subprocess.PIPE) as process:
            try:
                for timestamp, jpeg in frames:
                    if timestamp in samples:
                        sample = samples[timestamp]
                    with Image.open(BytesIO(jpeg)) as frame:
                        image = frame.convert("RGB")
                    if image.size != (width, height):
                        raise ValueError("Video export requires a constant frame size in each clip")
                    draw = ImageDraw.Draw(image)
                    for box in motion_boxes(sample, width, height):
                        draw.rectangle(box, outline="#48ff79", width=stroke)
                    label = f"{timestamp / 1e6:06.2f}s"
                    if sample and sample["result"] == "analyzed":
                        state = "MOTION" if sample["motion"] else "No motion"
                        label += f"  {state}  {sample['area']:.1f}%"
                        if timestamp != sample["capture_us"]:
                            label += "  (held)"
                    else:
                        label += "  Waiting for analysis" if not sample or sample["result"] == "priming" else "  Decode failed"
                    x0, y0, x1, y1 = draw.textbbox((6, 6), label, font=font)
                    draw.rectangle((2, 2, x1 + 4, y1 + 4), fill="#101818")
                    draw.text((6, 6), label, font=font, fill="white")
                    process.stdin.write(image.tobytes())
            finally:
                process.stdin.close()
            if process.wait():
                raise ValueError(f"FFmpeg failed to write {path}")
        target.replace(path)
