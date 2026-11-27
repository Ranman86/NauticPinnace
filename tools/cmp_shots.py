#!/usr/bin/env python3
"""Pixel compare of two simulator shot sets, or of two single shots.

The simulator's --shots DIR run writes one BMP per screen and overlay. With
--freeze-demo (plus --set / --ais-seed where needed) two runs of the same
build give the same pixels, so a compare of an old and a new build shows
exactly what a code change did to the picture:

    python -I tools/cmp_shots.py OLD_DIR NEW_DIR
    python -I tools/cmp_shots.py OLD_DIR NEW_DIR --region 88,0,688,600
    python -I tools/cmp_shots.py old.bmp new.png

Directories are matched by file name (*.bmp and *.png, not recursive). Each
pair gets one line: identical, or the number of differing pixels and the
bounding box of the difference. A file present on one side only is listed
as well.

--region x0,y0,x1,y1 restricts the compare to that box in image pixels, with
x1/y1 exclusive like a PIL box: 88,0,688,600 is the 600x600 instrument block
of simulator_7b in landscape (x 88..687). Without it the whole frame is
compared, which is what the 480x480 simulator wants. The box is clipped to
the image. Bounding boxes are printed in the same convention, in full-image
coordinates.

Only RGB is compared: the alpha byte of the sim's 32-bit BMPs carries no
picture.

Exit status: 0 only if at least one pair was compared, every compared pair
is identical (within the region) and no file is missing on either side;
1 otherwise; 2 for bad arguments or an image that cannot be read.
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    sys.stderr.write('Pillow not found - run:  pip install Pillow\n')
    sys.exit(2)   # a tool error, not "images differ"

EXTS = ('.bmp', '.png')


def parse_region(text):
    try:
        x0, y0, x1, y1 = (int(v) for v in text.split(','))
    except ValueError:
        raise argparse.ArgumentTypeError('expected x0,y0,x1,y1, got %r' % text)
    if x1 <= x0 or y1 <= y0:
        raise argparse.ArgumentTypeError('empty region %r (x1 and y1 are exclusive)' % text)
    return x0, y0, x1, y1


def shot_names(folder):
    return {n for n in os.listdir(folder)
            if n.lower().endswith(EXTS) and os.path.isfile(os.path.join(folder, n))}


def load_rgb(path):
    with Image.open(path) as im:
        return im.convert('RGB')


def compare(path_a, path_b, region):
    """(identical?, description) for one pair of images."""
    a, b = load_rgb(path_a), load_rgb(path_b)
    if a.size != b.size:
        return False, 'size %dx%d vs %dx%d' % (a.size + b.size)
    box = (0, 0, a.width, a.height)
    if region:
        box = (max(region[0], 0), max(region[1], 0),
               min(region[2], a.width), min(region[3], a.height))
        if box[2] <= box[0] or box[3] <= box[1]:
            return False, 'region lies outside the %dx%d image' % a.size
        a, b = a.crop(box), b.crop(box)
    # Per-channel difference, folded into one band that is non-zero wherever
    # any channel differs.
    r, g, bl = ImageChops.difference(a, b).split()
    mask = ImageChops.lighter(ImageChops.lighter(r, g), bl)
    bbox = mask.getbbox()
    if bbox is None:
        return True, 'identical'
    count = a.width * a.height - mask.histogram()[0]
    x0, y0, x1, y1 = bbox
    return False, '%d px differ, bbox %d,%d,%d,%d' % (
        count, x0 + box[0], y0 + box[1], x1 + box[0], y1 + box[1])


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Pixel compare of simulator shots: two directories '
                    '(matched by file name) or two image files.')
    ap.add_argument('a', help='reference: directory or image (e.g. the old build)')
    ap.add_argument('b', help='directory or image to compare with it')
    ap.add_argument('--region', type=parse_region, metavar='x0,y0,x1,y1',
                    help='compare only this box, x1/y1 exclusive (e.g. 88,0,688,600)')
    args = ap.parse_args(argv)

    if os.path.isdir(args.a) and os.path.isdir(args.b):
        names_a, names_b = shot_names(args.a), shot_names(args.b)
        pairs = [(n, os.path.join(args.a, n), os.path.join(args.b, n))
                 for n in sorted(names_a & names_b)]
        one_sided = ([(n, 'only in A') for n in sorted(names_a - names_b)] +
                     [(n, 'only in B') for n in sorted(names_b - names_a)])
    elif os.path.isfile(args.a) and os.path.isfile(args.b):
        pairs = [('%s <> %s' % (args.a, args.b), args.a, args.b)]
        one_sided = []
    else:
        ap.error('A and B must be two directories or two image files')

    if args.region:
        print('region %d,%d,%d,%d' % args.region)
    same = differ = errors = 0
    for name, pa, pb in pairs:
        try:
            ok, text = compare(pa, pb, args.region)
        except OSError as e:          # PIL raises OSError subclasses for bad files
            errors += 1
            print('  ERROR      %s: %s' % (name, e))
            continue
        if ok:
            same += 1
            print('  identical  %s' % name)
        else:
            differ += 1
            print('  DIFFERENT  %s: %s' % (name, text))
    for name, side in one_sided:
        print('  %-9s  %s' % (side.upper(), name))

    print('%d compared: %d identical, %d different%s%s' % (
        len(pairs), same, differ,
        ', %d unreadable' % errors if errors else '',
        ', %d on one side only' % len(one_sided) if one_sided else ''))
    if errors:
        return 2
    if differ or one_sided or not pairs:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
