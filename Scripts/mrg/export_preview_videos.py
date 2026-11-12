"""Encode continuous beauty EXR prefixes as display-referred MP4 previews."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--project', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--ffmpeg', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--fps', type=float, default=30)
    parser.add_argument('--crf', type=int, default=18)
    parser.add_argument('--threads', type=int, default=8)
    parser.add_argument('--scene')
    parser.add_argument('--spp')
    parser.add_argument('--decoder', choices=['ffmpeg', 'openexr'], default='ffmpeg')
    args = parser.parse_args()
    root = args.project.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    ffmpeg = str(args.ffmpeg.resolve())
    transform = ('format=gbrpf32le,'
                 'setparams=range=full:color_primaries=bt709:color_trc=linear:colorspace=gbr,'
                 'tonemap=tonemap=reinhard:param=0.5:desat=0:peak=100,'
                 'zscale=transfer=bt709:primaries=bt709:matrix=bt709:range=limited,'
                 'format=yuv420p')
    report = {'started': datetime.now().astimezone().isoformat(), 'fps': args.fps,
              'transform': transform, 'ffmpeg': subprocess.check_output([ffmpeg, '-version'], text=True).splitlines()[0],
              'purpose': 'Lossy SDR viewing previews, not training targets. Fixed transform across all sequences.',
              'videos': []}
    manifest = output / 'manifest.json'
    if manifest.exists():
        report = json.loads(manifest.read_text(encoding='utf-8'))
        if report['fps'] != args.fps or report['transform'] != transform:
            raise ValueError('Existing manifest uses different display settings')
    for beauty in sorted((root / 'Datasets').glob('LS_*/*spp/beauty')):
        if args.scene and beauty.parent.parent.name != args.scene:
            continue
        if args.spp and beauty.parent.name != args.spp:
            continue
        files = {int(p.stem): p for p in beauty.glob('*.exr') if p.stem.isdigit()}
        if not files:
            continue
        first = min(files)
        stop = first
        while stop in files and files[stop].stat().st_size > 0:
            stop += 1
        if stop == first:
            continue
        scene, spp = beauty.parent.parent.name, beauty.parent.name
        name = f'{scene}_{spp}_beauty'
        video = output / f'{name}.mp4'
        command = [ffmpeg, '-hide_banner', '-nostdin', '-n', '-xerror', '-threads', str(args.threads),
                   '-framerate', str(args.fps), '-start_number', str(first), '-i', str(beauty / '%04d.exr'),
                   '-frames:v', str(stop - first), '-an', '-filter_threads', str(args.threads), '-vf', transform,
                   '-c:v', 'libx264', '-threads', str(args.threads), '-preset', 'medium', '-crf', str(args.crf),
                   '-pix_fmt', 'yuv420p', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-colorspace', 'bt709',
                   '-color_range', 'tv', '-movflags', '+faststart', '-progress', 'pipe:1', str(video)]
        print(f'Encoding {scene}/{spp}: {first:04d}-{stop-1:04d} ({stop-first} frames)', flush=True)
        with (output / f'{name}.encode.log').open('w', encoding='utf-8') as log:
            if args.decoder == 'openexr':
                import OpenEXR
                import Imath
                sample = OpenEXR.InputFile(str(files[first]))
                window = sample.header()['dataWindow']
                sample.close()
                width, height = window.max.x-window.min.x+1, window.max.y-window.min.y+1
                input_end = command.index('-frames:v')
                command = command[:command.index('-framerate')] + [
                    '-f', 'rawvideo', '-pixel_format', 'gbrpf32le', '-video_size', f'{width}x{height}',
                    '-framerate', str(args.fps), '-i', 'pipe:0'] + command[input_end:]
                with (output / f'{name}.progress.log').open('w', encoding='utf-8') as progress:
                    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=progress, stderr=log)
                    try:
                        for frame in range(first, stop):
                            image = OpenEXR.InputFile(str(files[frame]))
                            try:
                                for channel in 'GBR':
                                    process.stdin.write(image.channel(channel, Imath.PixelType(Imath.PixelType.FLOAT)))
                            finally:
                                image.close()
                            if (frame-first+1) % 100 == 0:
                                print(f'OpenEXR decoded {frame-first+1}/{stop-first}', flush=True)
                        process.stdin.close()
                        returncode = process.wait()
                    except BaseException:
                        process.kill()
                        process.wait()
                        raise
            else:
                result = subprocess.run(command, stdout=subprocess.PIPE, stderr=log, text=True)
                returncode = result.returncode
        if returncode:
            raise RuntimeError(f'Encoding failed: see {name}.encode.log')
        encode_log = (output / f'{name}.encode.log').read_text(encoding='utf-8', errors='replace')
        if 'decode_block() failed' in encode_log:
            raise RuntimeError('FFmpeg EXR decoder reported errors; retry using --decoder openexr')
        # Decode the entire result and check its frame count, not just container metadata.
        verification = [ffmpeg, '-hide_banner', '-nostdin', '-v', 'error', '-xerror', '-i', str(video),
                        '-map', '0:v:0', '-f', 'null', '-progress', 'pipe:1', '-']
        check = subprocess.run(verification, capture_output=True, text=True, check=True)
        counts = re.findall(r'^frame=(\d+)', check.stdout, re.M)
        if not counts or int(counts[-1]) != stop - first:
            raise RuntimeError(f'Frame count mismatch in {video}')
        subprocess.run([ffmpeg, '-hide_banner', '-nostdin', '-v', 'error', '-n', '-i', str(video),
                        '-frames:v', '1', '-vf', 'scale=640:-2', str(output / f'{name}.jpg')], check=True)
        row = {'scene': scene, 'spp': spp, 'source': beauty.relative_to(root).as_posix(),
               'first': first, 'last': stop - 1, 'frames': stop - first, 'duration_seconds': (stop-first)/args.fps,
               'path': video.relative_to(root).as_posix(), 'bytes': video.stat().st_size,
               'decoder': args.decoder,
               'decoded_frames_verified': int(counts[-1]), 'command': command, 'verification_command': verification}
        if manifest.exists():
            report = json.loads(manifest.read_text(encoding='utf-8'))
        report['videos'] = [v for v in report['videos'] if (v['scene'], v['spp']) != (scene, spp)]
        report['videos'].append(row)
        report['finished'] = datetime.now().astimezone().isoformat()
        (output / 'manifest.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'Verified {video.name}: {stop-first} frames, {video.stat().st_size / 2**20:.1f} MiB', flush=True)


if __name__ == '__main__':
    main()
