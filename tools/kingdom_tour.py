"""Bounded Vulkan screenshot tour using private game data and a completed-save copy.

Requires Pillow. Output contains local screenshots/logs/saves, never commit it.
Use --baseline with an equivalent approved tour directory for visual comparison.
"""
import argparse
import hashlib
import html
import json
import os
from pathlib import Path
import re
import signal
import shutil
import subprocess
import time

from PIL import Image, ImageChops, ImageEnhance, ImageStat


KINGDOMS = {
    'cap': 'CapWorldHomeStage', 'cascade': 'WaterfallWorldHomeStage',
    'sand': 'SandWorldHomeStage', 'lake': 'LakeWorldHomeStage',
    'wooded': 'ForestWorldHomeStage', 'cloud': 'CloudWorldHomeStage',
    'lost': 'ClashWorldHomeStage', 'metro': 'CityWorldHomeStage',
    'snow': 'SnowWorldHomeStage', 'seaside': 'SeaWorldHomeStage',
    'luncheon': 'LavaWorldHomeStage', 'ruined': 'BossRaidWorldHomeStage',
    'bowser': 'SkyWorldHomeStage', 'moon': 'MoonWorldHomeStage',
    'mushroom': 'PeachWorldHomeStage', 'dark': 'Special1WorldHomeStage',
    'darker': 'Special2WorldHomeStage',
}
ARRIVAL = re.compile(r'\[warp\] stage scene \w+ running at frame (\d+) stage "([^"]+)"')


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def stop_test_process(proc, log):
    # Windows can re-exec for the guest address reservation: its parent waits
    # for that child. Kill only this test's tree, not every running game.
    if os.name == 'nt':
        subprocess.run(['taskkill', '/PID', str(proc.pid), '/T', '/F'], stdout=log,
                       stderr=subprocess.STDOUT, check=False)
    else:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    proc.wait()


def comparison(image, baseline, difference):
    with Image.open(image) as a, Image.open(baseline) as b:
        a, b = a.convert('RGB'), b.convert('RGB')
        if a.size != b.size:
            return {'comparison': 'size_mismatch', 'size': a.size, 'baseline_size': b.size}
        diff = ImageChops.difference(a, b)
        mean = sum(ImageStat.Stat(diff).mean) / (3 * 255)
        # A small broken texture can disappear in a whole-screen average.
        local = max(sum(ImageStat.Stat(diff.crop((x, y, min(x + 32, a.width), min(y + 32, a.height)))).mean)
                    / (3 * 255) for y in range(0, a.height, 32) for x in range(0, a.width, 32))
        # Save amplified differences even below the threshold for human review.
        ImageEnhance.Contrast(diff).enhance(4).save(difference)
        return {'comparison': 'review' if mean > 0.01 or local > 0.08 else 'within_threshold',
                'mean_absolute_difference': mean, 'max_tile_difference': local}


def write_report(out, report):
    (out / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    rows = []
    for r in report['results']:
        key = r['kingdom']
        rows.append(f'<tr><td>{html.escape(key)}</td><td>{html.escape(r["status"])}</td>'
                    f'<td>{html.escape(r.get("comparison", "not compared"))}</td>'
                    f'<td><a href="{key}.png">capture</a> · <a href="{key}.log">log</a>'
                    f' · <a href="{key}-diff.png">difference (if compared)</a></td></tr>')
    (out / 'report.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>Kingdom tour</title>'
        '<style>body{font:16px system-ui;margin:2rem}td,th{padding:.6rem;text-align:left}</style>'
        '<h1>Kingdom tour</h1><p>Images require human review. Animated effects and timing '
        'can produce differences; small differences do not prove correctness.</p>'
        '<table><tr><th>Kingdom</th><th>Run</th><th>Comparison</th><th>Artifacts</th></tr>'
        + ''.join(rows) + '</table>', encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--exe', required=True, type=Path)
    ap.add_argument('--data-root', required=True, type=Path, help='PRIVATE COPY of working game-data folder')
    ap.add_argument('--save', required=True, type=Path, help='completed-save directory containing save_0')
    ap.add_argument('--out', required=True, type=Path, help='new output directory; existing paths refused')
    ap.add_argument('--baseline', type=Path, help='approved tour output from the last release with identical test hooks/settings')
    ap.add_argument('--kingdoms', nargs='+', choices=KINGDOMS, default=list(KINGDOMS))
    ap.add_argument('--warp-frame', type=int, default=1700)
    ap.add_argument('--frames', type=int, default=4500)
    ap.add_argument('--settle', type=int, default=600, help='minimum frames after destination arrival')
    ap.add_argument('--timeout', type=float, default=180, help='seconds per kingdom')
    ap.add_argument('--frame-rate', choices=(60, 120), type=int, default=60,
                    help='120 is experimental; use a larger frame budget for equivalent settling time')
    ap.add_argument('--visible', action='store_true')
    args = ap.parse_args()
    exe, data, save, out = (p.resolve() for p in (args.exe, args.data_root, args.save, args.out))
    if not exe.is_file() or not (data / 'extracted/exefs/main').is_file() or not (save / 'save_0').is_dir():
        ap.error('Need a built executable, copied extracted game data and a save containing save_0')
    if out.exists() or out == data or out.is_relative_to(save) or save.is_relative_to(out):
        ap.error('Output must be a new separate directory')
    if args.warp_frame < 1 or args.settle < 1 or args.frames <= args.warp_frame + args.settle or args.timeout <= 0:
        ap.error('Invalid frame/timeout budget')
    out.mkdir(parents=True)
    settings = out / 'tour.ini'
    settings.write_text('window_mode = windowed\ndisplay_resolution = 1080p\n'
                        f'resolution = match\nvolume = 0\nframe_rate = {args.frame_rate}\nframe_interp = 0\n'
                        'temporal_backend = dlss\ndlss_mode = off\nantialias = off\n'
                        'upscaler = bilinear\naniso = 16\nmotion = 0\nvibration = 0\nmouse_camera = 0\n'
                        'skip_intro = 1\ncheck_updates = 0\n', encoding='ascii')
    report = {'exe_sha256': sha256(exe), 'game_main_sha256': sha256(data / 'extracted/exefs/main'),
              'settings': settings.read_text(), 'frames': args.frames,
              'warp_frame': args.warp_frame, 'settle_frames': args.settle, 'results': []}
    report['save_sha256'] = {str(p.relative_to(save)): sha256(p) for p in sorted(save.rglob('*')) if p.is_file()}
    if args.baseline:
        reference = json.loads((args.baseline / 'report.json').read_text(encoding='utf-8'))
        for field in ('game_main_sha256', 'settings', 'frames', 'warp_frame', 'settle_frames', 'save_sha256'):
            if reference.get(field) != report[field]:
                ap.error(f'Baseline differs in {field}; capture an equivalent baseline first')
    base_env = {k: v for k, v in os.environ.items() if not k.upper().startswith('ODYSSEY_')}
    for key in args.kingdoms:
        stage = KINGDOMS[key]
        image = out / f'{key}.png'
        private_save = out / f'{key}-save'
        shutil.copytree(save, private_save)
        env = dict(base_env, ODYSSEY_DATA_ROOT=str(data), ODYSSEY_SAVE_DIR=str(private_save),
                   ODYSSEY_SETTINGS=str(settings), ODYSSEY_NO_UPDATE_CHECK='1', ODYSSEY_PREPARE='0',
                   ODYSSEY_WINDOW='1280x720', ODYSSEY_NO_USER_INPUT='1',
                   ODYSSEY_INPUT='900:A:8,1050:A:8,1200:A:8,1350:A:8',
                   ODYSSEY_WARP=f'world:{stage}@{args.warp_frame}', ODYSSEY_LOG_STAGES='1',
                   ODYSSEY_WINDOW_SHOT=str(args.frames), ODYSSEY_WINDOW_SHOT_PATH=str(image),
                   ODYSSEY_EXIT_AFTER_SHOT='1', ODYSSEY_LINEAR_WRITEBACK='0')
        if not args.visible:
            env['ODYSSEY_HIDDEN'] = '1'
        result = {'kingdom': key, 'stage': stage, 'status': 'failed'}
        print(f'{key}: starting', flush=True)
        started = time.monotonic()
        with (out / f'{key}.log').open('wb') as log:
            flags = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
            with subprocess.Popen([str(exe)], cwd=data, env=env, stdout=log, stderr=subprocess.STDOUT,
                                  creationflags=flags, start_new_session=os.name != 'nt') as proc:
                try:
                    result['exit_code'] = proc.wait(timeout=args.timeout)
                except subprocess.TimeoutExpired:
                    stop_test_process(proc, log)
                    result['status'] = 'timeout'
                except KeyboardInterrupt:
                    stop_test_process(proc, log)
                    raise
        result['seconds'] = round(time.monotonic() - started, 2)
        log = (out / f'{key}.log').read_text(encoding='utf-8', errors='replace')
        arrivals = [(int(frame), name) for frame, name in ARRIVAL.findall(log)]
        reached = [frame for frame, name in arrivals if name == stage and frame >= args.warp_frame]
        result['arrivals'] = arrivals
        requested = bool(re.search(r'\[warp\] frame \d+: ' + re.escape(stage) + r' .* -> requested', log))
        result['warp_requested'] = requested
        if (result.get('exit_code') == 0 and requested and image.is_file() and reached and
                arrivals[-1][1] == stage and reached[-1] + args.settle <= args.frames):
            with Image.open(image) as shot:
                shot.verify()
            result['status'] = 'captured'
            baseline = args.baseline.resolve() / f'{key}.png' if args.baseline else None
            if baseline and baseline.is_file():
                result.update(comparison(image, baseline, out / f'{key}-diff.png'))
            else:
                result['comparison'] = 'missing_baseline' if args.baseline else 'no_baseline'
        report['results'].append(result)
        write_report(out, report)
        print(f'{key}: {result["status"]}, {result.get("comparison", "not compared")}', flush=True)
    failed = any(r['status'] != 'captured' for r in report['results'])
    review = any(r.get('comparison') in ('review', 'size_mismatch', 'missing_baseline') for r in report['results'])
    return 1 if failed else 2 if review else 0


if __name__ == '__main__':
    raise SystemExit(main())
