"""Inspect raw EXRs and write explicitly transformed browsing previews."""
import argparse
import json
from pathlib import Path
import OpenEXR
import numpy as np
from PIL import Image, ImageDraw

parser = argparse.ArgumentParser()
parser.add_argument('dataset', type=Path)
parser.add_argument('--depth-max-m', type=float, default=200)
args = parser.parse_args()
out = args.dataset / 'audit'
out.mkdir(exist_ok=True)
records, comparisons, tiles = [], [], []

def read(path):
    f = OpenEXR.File(str(path), separate_channels=True)
    return f, np.stack([f.channels()[c].pixels for c in 'RGB'], -1).astype(np.float32)

def srgb(x):
    x = np.maximum(x, 0)
    return np.where(x <= .0031308, x * 12.92, 1.055 * x ** (1 / 2.4) - .055)

for path in sorted(args.dataset.glob('*spp/**/*.exr')):
    f, a = read(path)
    channel = path.parent.name
    rec = {'file': str(path.relative_to(args.dataset)), 'shape': list(a.shape),
           'dtype': str(f.channels()['R'].pixels.dtype), 'nonfinite': int((~np.isfinite(a)).sum()),
           'min': np.min(a, axis=(0, 1)).tolist(), 'max': np.max(a, axis=(0, 1)).tolist(),
           'std': np.std(a, axis=(0, 1), dtype=np.float64).tolist(),
           'metadata': {k: str(v) for k,v in f.header().items() if k.startswith('unreal/') and not k.startswith(('unreal/system/', 'unreal/stats/'))}}
    records.append(rec)
    if channel == 'world_normal':
        rec['decoded_normal_length_quantiles'] = np.quantile(np.linalg.norm(a*2-1, axis=-1), [0,.01,.5,.99,1]).tolist()
    if path.parts[-4] == '1spp' and channel != 'beauty':
        peer = args.dataset / '128spp' / path.relative_to(args.dataset / '1spp')
        if peer.exists():
            _, b = read(peer)
            diff = abs(a - b)
            comparisons.append({'channel': channel, 'frame': path.stem,
                                'equal_fraction': float(np.mean(a == b)),
                                'mae': float(diff.mean(dtype=np.float64)), 'max_abs': float(diff.max()),
                                'rgb_mae': diff.mean(axis=(0,1), dtype=np.float64).tolist(),
                                'rgb_max_abs': diff.max(axis=(0,1)).tolist()})
    if not path.name.endswith('0000.exr'):
        continue
    label = path.relative_to(args.dataset).parts[0] + ' / ' + channel
    if channel == 'beauty':
        a = srgb(a / (1 + np.maximum(a, 0)))
        label += ' | Reinhard + sRGB preview'
    elif channel == 'base_color':
        a = srgb(a)
        label += ' | sRGB preview'
    elif channel == 'world_depth':
        z = a[..., 0] / 100
        valid = np.isfinite(z) & (z > 0) & (z < args.depth_max_m)
        lo, hi = np.quantile(z[valid], [.01, .99])
        a = np.where(valid, (np.clip(z, lo, hi)-lo)/(hi-lo), 0)
        label += f' | R: {lo:.1f}..{hi:.1f} m'
    elif channel == 'motion_vectors':
        label += ' | encoded RG, static = (0.5,0.5)'
    if a.ndim == 2:
        a = np.repeat(a[..., None], 3, -1)
    im = Image.fromarray((np.clip(np.nan_to_num(a), 0, 1)*255).astype('uint8'))
    im.thumbnail((640,360))
    tile = Image.new('RGB', (640,396), '#171a20')
    tile.paste(im, (0,36))
    ImageDraw.Draw(tile).text((10,10),label,fill='white')
    tiles.append(tile)
sheet = Image.new('RGB', (1920,396*((len(tiles)+2)//3)), '#171a20')
for i,t in enumerate(tiles):
    sheet.paste(t,((i%3)*640,(i//3)*396))
sheet.save(out/'preview.jpg', quality=93)
(out/'statistics.json').write_text(json.dumps({'files':records,'comparisons':comparisons},indent=2))
print(json.dumps({'count':len(records),'first_frames':[{k:v for k,v in r.items() if k!='metadata'} for r in records if r['file'].endswith('0000.exr')],'comparisons':comparisons},indent=2))
