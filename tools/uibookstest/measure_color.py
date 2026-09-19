"""Measure the book body-text glyph color in a PrintWindow BMP capture.

Usage:  python measure_color.py PATH [x0 y0 x1 y1]
Without a crop, it auto-detects the text cluster (dark pixels on the light
parchment page). With a crop it measures that rectangle. It reports:
  - size, a coarse histogram of the brightest clusters (parchment bg),
  - the "text" cluster = pixels markedly darker than the background,
  - the median + mean color of the text cluster, and a few darkest samples.
"""
import sys
import numpy as np
from PIL import Image, ImageFile

ImageFile.LOAD_TRUNCATED_IMAGES = True


def load_rgb(path):
    im = Image.open(path).convert("RGB")
    return np.asarray(im, dtype=np.int32)


def describe(arr, name):
    h, w, _ = arr.shape
    flat = arr.reshape(-1, 3)
    lum = flat.mean(axis=1)
    # Background = the big bright cluster. Text = markedly darker.
    bg_thresh = np.percentile(lum, 75)  # parchment sits near the bright end
    mask = lum < (bg_thresh - 25)
    text = flat[mask]
    print("\n=== %s  (%dx%d) ===" % (name, w, h))
    if text.shape[0] < 20:
        print("  not enough dark pixels in this region (text crop too wide/off?)")
        return
    med = np.median(text, axis=0).astype(int)
    mean = text.mean(axis=0).astype(int)
    darkest = text[np.argsort(text.mean(axis=1))[:50]]
    dmed = np.median(darkest, axis=0).astype(int)
    pct = 100.0 * text.shape[0] / flat.shape[0]
    print("  text pixels: %d (%.1f%% of region)" % (text.shape[0], pct))
    print("  text median   : #%02X%02X%02X" % tuple(med))
    print("  text mean     : #%02X%02X%02X" % tuple(mean))
    print("  50 darkest med: #%02X%02X%02X" % tuple(dmed))
    print("  bg(75pct) lum : %d" % int(bg_thresh))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    path = sys.argv[1]
    arr = load_rgb(path)
    if len(sys.argv) >= 6:
        x0, y0, x1, y1 = (int(v) for v in sys.argv[2:6])
        crop = arr[y0:y1, x0:x1]
        describe(crop, "%s crop[%d:%d,%d:%d]" % (path, x0, y0, x1, y1))
    else:
        describe(arr, path)


if __name__ == "__main__":
    main()
