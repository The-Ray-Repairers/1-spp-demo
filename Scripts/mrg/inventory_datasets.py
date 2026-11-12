"""Read-only EXR inventory. Requires OpenEXR; reports use project-relative paths.

Ranges are inferred from the union of available frames in each scene, not from
the Level Sequence asset. Structural checks do not replace full pixel decoding.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import csv
from datetime import datetime
import json
from pathlib import Path
import re
import shutil
import struct

import OpenEXR

CHANNELS = ('beauty', 'base_color', 'motion_vectors', 'roughness', 'world_depth', 'world_normal')


def inspect(path):
    result = {'status': 'ok'}
    before = path.stat()
    result['bytes'] = before.st_size
    if not before.st_size:
        return dict(result, status='empty')
    try:
        exr = OpenEXR.InputFile(str(path))
        try:
            header = exr.header()
            if not exr.isComplete():
                raise ValueError('OpenEXR reports incomplete chunk table')
            window = header['dataWindow']
            width = window.max.x - window.min.x + 1
            height = window.max.y - window.min.y + 1
            result['resolution'] = [width, height]
            result['channels'] = str(header['channels'])
            result['fps'] = header.get('unreal/frameRate', b'').decode()
        finally:
            exr.close()
        # Validate all scanline chunk extents, including chunks whose offsets
        # exist but whose payload was truncated by a failed disk write.
        with path.open('rb') as stream:
            magic, version = struct.unpack('<II', stream.read(8))
            if magic != 20000630 or version & ~0x4ff:
                raise ValueError('unsupported EXR layout (requires single-part scanline)')

            def cstring():
                value = bytearray()
                while True:
                    char = stream.read(1)
                    if not char or len(value) > 65536:
                        raise ValueError('invalid EXR header')
                    if char == b'\0':
                        return bytes(value)
                    value.extend(char)

            compression = None
            while True:
                name = cstring()
                if not name:
                    break
                cstring()
                length, = struct.unpack('<I', stream.read(4))
                if length > before.st_size - stream.tell():
                    raise ValueError('header extends past EOF')
                if name == b'compression':
                    compression = stream.read(length)[0]
                else:
                    stream.seek(length, 1)
            block_lines = {0: 1, 1: 1, 2: 1, 3: 16, 4: 32, 5: 16, 6: 32, 7: 32, 8: 32, 9: 256}[compression]
            count = (height + block_lines - 1) // block_lines
            offsets = struct.unpack('<' + 'Q' * count, stream.read(count * 8))
            payload_start = stream.tell()
            for offset in offsets:
                if offset < payload_start or offset + 8 > before.st_size:
                    raise ValueError('invalid chunk offset')
                stream.seek(offset)
                y, length = struct.unpack('<iI', stream.read(8))
                if not window.min.y <= y <= window.max.y or offset + 8 + length > before.st_size:
                    raise ValueError('truncated or invalid chunk')
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError('file changed during audit')
    except Exception as error:
        result.update(status='invalid', error=str(error))
    return result


def write_csv(path, rows, fields):
    with path.open('w', encoding='utf-8-sig', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=4)
    args = parser.parse_args()
    root = args.project.resolve()
    started = datetime.now().astimezone().isoformat()
    records = []
    for scene in sorted((root / 'Datasets').iterdir()):
        if not scene.is_dir():
            continue
        for spp in sorted(scene.glob('*spp')):
            for path in sorted(spp.rglob('*.exr')):
                match = re.search(r'(\d+)\.exr$', path.name)
                if not match:
                    continue
                records.append({'scene': scene.name, 'spp': spp.name, 'channel': path.parent.name,
                                'frame': int(match[1]), 'path': path.relative_to(root).as_posix()})
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for record, checked in zip(records, pool.map(inspect, (root / r['path'] for r in records))):
            record.update(checked)
    index = {(r['scene'], r['spp'], r['channel'], r['frame']): r for r in records}
    if len(index) != len(records):
        raise ValueError('duplicate scene/spp/channel/frame keys')
    summaries, issues, pairs, samples, aliases = [], [], [], [], []
    for scene in sorted({r['scene'] for r in records}):
        frames = [r['frame'] for r in records if r['scene'] == scene]
        inferred = range(min(frames), max(frames) + 1)
        spps = sorted({r['spp'] for r in records if r['scene'] == scene})
        complete = {}
        for spp in spps:
            valid_frames = set(inferred)
            for channel in CHANNELS:
                group = [r for r in records if (r['scene'], r['spp'], r['channel']) == (scene, spp, channel)]
                good = {r['frame'] for r in group if r['status'] == 'ok'}
                valid_frames &= good
                for frame in inferred:
                    row = index.get((scene, spp, channel, frame))
                    if row is None or row['status'] != 'ok':
                        issues.append({'scene': scene, 'spp': spp, 'channel': channel, 'frame': frame,
                                       'status': row['status'] if row else 'missing_in_observed_scene_range',
                                       'path': row['path'] if row else '', 'error': row.get('error', '') if row else ''})
                summaries.append({'scene': scene, 'spp': spp, 'channel': channel, 'files': len(group),
                                  'structurally_valid': len(good), 'empty': sum(r['status'] == 'empty' for r in group),
                                  'invalid': sum(r['status'] == 'invalid' for r in group),
                                  'bytes': sum(r['bytes'] for r in group),
                                  'observed_scene_first': inferred.start, 'observed_scene_last': inferred.stop - 1})
                if good:
                    selected = {min(good), sorted(good)[len(good) // 2], max(good)}
                    for frame in sorted(selected):
                        row = index[scene, spp, channel, frame]
                        try:
                            exr = OpenEXR.InputFile(str(root / row['path']))
                            try:
                                for name in exr.header()['channels']:
                                    exr.channel(name)
                            finally:
                                exr.close()
                            status = 'decode_ok'
                        except Exception as error:
                            status = str(error)
                            valid_frames.discard(frame)
                        samples.append({'path': row['path'], 'status': status})
            complete[spp] = sorted(valid_frames)
        if '1spp' in complete and '128spp' in complete:
            for frame in sorted(set(complete['1spp']) & set(complete['128spp'])):
                row = {'scene': scene, 'frame': frame}
                for spp in ('1spp', '128spp'):
                    for channel in CHANNELS:
                        row[f'{spp}_{channel}'] = index[scene, spp, channel, frame]['path']
                pairs.append(row)
        aliases.append({'scene': scene, 'complete_frames': complete})
    hardlinks = {'source_exists': (root / 'Saved/DatasetLinear').exists(), 'matched': 0, 'different': 0, 'absent': 0}
    for row in records:
        source = root / row['path'].replace('Datasets/', 'Saved/DatasetLinear/', 1).replace('/auxiliary/', '/aux/')
        if source.exists():
            a, b = (root / row['path']).stat(), source.stat()
            same = (a.st_dev, a.st_ino, a.st_size) == (b.st_dev, b.st_ino, b.st_size)
            hardlinks['matched' if same else 'different'] += 1
        else:
            hardlinks['absent'] += 1
    diagnostic = []
    for parent in (root / 'Saved/Diagnostics', root / 'Saved/Automation'):
        for directory in sorted(parent.glob('render_validation_*' if parent.name == 'Diagnostics' else '*')):
            files = list(directory.rglob('*.exr'))
            diagnostic.append({'path': directory.relative_to(root).as_posix(), 'exrs': len(files),
                               'bytes': sum(p.stat().st_size for p in files)})
    args.output.mkdir(parents=True, exist_ok=True)
    write_csv(args.output / 'channels.csv', summaries, list(summaries[0]))
    write_csv(args.output / 'issues.csv', issues, ['scene', 'spp', 'channel', 'frame', 'status', 'path', 'error'])
    write_csv(args.output / 'paired_frames.csv', pairs, ['scene', 'frame'] + [f'{s}_{c}' for s in ('1spp', '128spp') for c in CHANNELS])
    report = {'started': started, 'finished': datetime.now().astimezone().isoformat(), 'project': str(root),
              'range_basis': 'union of observed frame indices per scene; sequence assets not inspected',
              'disk_free_bytes': shutil.disk_usage(root).free, 'summaries': summaries, 'scenes': aliases,
              'hardlinks': hardlinks, 'diagnostic_outputs': diagnostic, 'pixel_decode_samples': samples,
              'records': records}
    (args.output / 'audit.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'files': len(records), 'issues': len(issues), 'pairs': len(pairs),
                      'samples': len(samples), 'decode_failures': sum(s['status'] != 'decode_ok' for s in samples),
                      'hardlinks': hardlinks, 'output': str(args.output)}, ensure_ascii=False))


if __name__ == '__main__':
    main()
