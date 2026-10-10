#!/usr/bin/env python3
"""Regenerates the icon files used by the player from assets/logo.png (needs Pillow)."""
import os
from PIL import Image

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
src = Image.open(os.path.join(root, 'assets', 'logo.png')).convert('RGB')
out = os.path.join(root, 'player', 'assets')
os.makedirs(out, exist_ok=True)

src.resize((512, 512), Image.LANCZOS).save(os.path.join(out, 'logo-512.png'), optimize=True)
# the window icon is sent to the X server in one request: 128x128 stays well below its size limit
src.resize((128, 128), Image.LANCZOS).save(os.path.join(out, 'icon-128.png'), optimize=True)
sizes = [16, 24, 32, 48, 64, 128, 256]   # Windows picks the best one
src.save(os.path.join(out, 'icon.ico'), sizes=[(s, s) for s in sizes])
print('wrote', sorted(os.listdir(out)))

# macOS application icon (used for the .app bundle made by the release workflow)
src.resize((1024, 1024), Image.LANCZOS).save(
    os.path.join(root, 'assets', 'AppIcon.icns'),
    sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
print('wrote assets/AppIcon.icns')
