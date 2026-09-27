from pathlib import Path
import json
import sys
import gi

gi.require_version('GdkPixbuf', '2.0')
from gi.repository import GdkPixbuf

root = Path(sys.argv[1])
for baseline in sorted(root.glob('*-auto-baseline.png')):
    first = GdkPixbuf.Pixbuf.new_from_file(str(baseline))
    a = first.get_pixels()
    for stage in ('inserted', 'selected', 'deleted'):
        second = GdkPixbuf.Pixbuf.new_from_file(str(baseline).replace('-baseline.png', f'-{stage}.png'))
        b = second.get_pixels()
        assert (first.get_width(), first.get_height(), first.get_n_channels(), first.get_rowstride()) == (
            second.get_width(), second.get_height(), second.get_n_channels(), second.get_rowstride())
        points = []
        channels = first.get_n_channels()
        for y in range(first.get_height()):
            for x in range(first.get_width()):
                i = y * first.get_rowstride() + x * channels
                if a[i:i+channels] != b[i:i+channels]:
                    points.append((x, y))
        print(json.dumps({'image': baseline.name, 'stage': stage, 'changed_pixels': len(points),
            'bounds': [min(x for x, y in points), min(y for x, y in points),
                       max(x for x, y in points), max(y for x, y in points)] if points else None}))
