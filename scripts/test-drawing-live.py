#!/usr/bin/env python3
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from PIL import Image, ImageChops, ImageDraw, ImageStat

spec = importlib.util.spec_from_file_location('audio_live', Path(__file__).with_name('test-audio-live.py'))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)

DESCRIPTION = ('Play Drawing transitions through the staged plugin and Paper in a private KWin '
               'and Plasma session, capturing composed output frames while the strokes draw.')
SOCKET = 'drawing-test'
STREAM_FPS = 30
SETTLE_SECONDS = 2.5
CAPTURE_TAIL_SECONDS = 1.5
PRELUDE_LIMIT_SECONDS = 30
RESTART_WATCH_SECONDS = 8
VIDEO_FRAMES = 48
VIDEO_RATE = 24
BAR_HEIGHT = 0.08
MOVING_DELTA = 4
PAPER_TOLERANCE = 30


def main():
    parser = argparse.ArgumentParser(description=DESCRIPTION)
    parser.add_argument('--plugin', required=True, type=Path)
    parser.add_argument('--paper', required=True, type=Path)
    parser.add_argument('--artifact', required=True, type=Path)
    parser.add_argument('--from-image', required=True, type=Path)
    parser.add_argument('--results', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--size', default='1920x1080')
    parser.add_argument('--duration-ms', type=int, default=6500)
    args = parser.parse_args()
    plugin_source, paper = args.plugin.resolve(strict=True), args.paper.resolve(strict=True)
    artifact = args.artifact.resolve(strict=True)
    reference = Path(json.loads(artifact.read_text())['source']['path'])
    width, height = (int(side) for side in args.size.split('x'))
    root, runtime = helpers.create_directories(args.results, args.runtime)
    source = Path(__file__).resolve().parents[1]
    module = root / 'qml/org/skwd/wallpaper'
    module.mkdir(parents=True)
    plugin = module / 'libskwdwallpaperplugin.so'
    shutil.copy2(plugin_source, plugin)
    shutil.copy2(source / 'qmldir', module / 'qmldir')
    data = root / 'data'
    shutil.copytree(source / 'wallpaper', data / 'plasma/wallpapers/org.skwd.wall.plasma')
    for name in ['config', 'cache', 'state', 'tmp', 'captures', 'empty-desktop']:
        (root / name).mkdir()
    (root / 'config/kwinrc').write_text('[Plugins]\nslideEnabled=false\nslidebackEnabled=false\n')
    env = dict(os.environ)
    for name in ['DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XAUTHORITY', 'NIRI_SOCKET',
                 'HYPRLAND_INSTANCE_SIGNATURE', 'SWAYSOCK', 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR']:
        env.pop(name, None)
    env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_DATA_HOME=str(data), XDG_CACHE_HOME=str(root / 'cache'),
               XDG_STATE_HOME=str(root / 'state'), TMPDIR=str(root / 'tmp'),
               XDG_DATA_DIRS=f'{data}:/usr/local/share:/usr/share',
               QML_IMPORT_PATH=str(root / 'qml'), QML2_IMPORT_PATH=str(root / 'qml'),
               XDG_CURRENT_DESKTOP='KDE', XDG_SESSION_DESKTOP='KDE', XDG_SESSION_TYPE='wayland',
               KDE_FULL_SESSION='true', KDE_SESSION_VERSION='6', QSG_RHI_BACKEND='opengl',
               QT_QPA_PLATFORM='wayland', QT_FORCE_STDERR_LOGGING='1',
               KWIN_SCREENSHOT_NO_PERMISSION_CHECKS='1', KWIN_WAYLAND_NO_PERMISSION_CHECKS='1',
               SKWD_VK_DECODE='sw', SKWD_WALL_LOG='info', GIO_USE_VFS='local',
               PULSE_SERVER=f'unix:{runtime}/unused-pulse',
               SKWD_WALL_VK_BIN=str(paper.parent / 'skwd-wall-vk'),
               SKWD_WALL_STILL_BIN=str(paper.parent / 'skwd-wall-still'))
    processes = []
    report = {'checks': [], 'captures': [], 'artifacts': {}}
    for path in [paper, paper.parent / 'skwd-wall-vk', plugin, artifact, reference]:
        report['artifacts'][str(path)] = hashlib.sha256(path.read_bytes()).hexdigest()

    def run(command, check=True, timeout=20):
        result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=timeout)
        if check and result.returncode:
            raise RuntimeError(f'{command}: {result.stderr} {result.stdout}')
        return result

    def start(command, name, stdout=None):
        logfile = open(root / f'{name}.log', 'wb')
        process = subprocess.Popen(command, env=env, stdout=stdout or logfile, stderr=logfile,
                                   start_new_session=True)
        processes.append(process)
        return process

    def wait(predicate, label, timeout=25):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.1)
        raise RuntimeError(f'timed out: {label}')

    def check(name, ok, detail=None):
        entry = {'name': name, 'passed': bool(ok), 'detail': detail}
        report['checks'].append(entry)
        print(json.dumps(entry), flush=True)
        if not ok:
            raise AssertionError(name)

    def script(js):
        return run(['qdbus6', 'org.kde.plasmashell', '/PlasmaShell',
                    'org.kde.PlasmaShell.evaluateScript', js], check=False)

    def preludes():
        found = []
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                if f'XDG_RUNTIME_DIR={runtime}'.encode() not in (entry / 'environ').read_bytes().split(b'\0'):
                    continue
                argv = (entry / 'cmdline').read_bytes().decode().strip('\0').split('\0')
                if '--preview-stream' in argv:
                    found.append(argv)
            except OSError:
                pass
        return found

    def capture(label, output):
        path = root / 'captures' / f'{label}-{output}.png'
        run([str(root / 'capture'), output, str(path)])
        return path

    serial = iter(range(1, 1000))

    def presentation():
        identifier = f'{os.getpid()}-{next(serial)}'
        directory = runtime / 'skwd-paper-plasma'
        directory.mkdir(mode=0o700, exist_ok=True)
        (directory / f'{identifier}.json').write_text('{"state":"pending"}')
        return identifier

    def presented(identifiers):
        states = []
        for identifier in identifiers:
            path = runtime / 'skwd-paper-plasma' / f'{identifier}.json'
            states.append(json.loads(path.read_text()).get('state') if path.exists() else None)
        return states

    def apply(outputs, media, transition=None):
        current = {}
        for name in outputs:
            assignment = {'outputs': [name], 'source': media, 'mute': True, 'volume': 80,
                          'fill_mode': 'fill', 'layer': 'background'}
            if transition:
                assignment['transition'] = transition
            current[name] = {'assignment': assignment, 'paper': str(paper), 'width': width,
                             'height': height, 'fps': STREAM_FPS, 'paused': False, 'manualPaused': False,
                             'presentationId': presentation()}
        js = ('var a=' + json.dumps(current, separators=(',', ':')) + ';'
              'desktops().forEach(function(d){d.currentConfigGroup=["General"];d.writeConfig("url",'
              + json.dumps((root / 'empty-desktop').as_uri()) + ');'
              'd.currentConfigGroup=["Wallpaper","org.skwd.wall.plasma","General"];'
              'd.writeConfig("Assignments",JSON.stringify(a));d.wallpaperPlugin="org.skwd.wall.plasma";});')
        result = script(js)
        check('assignments accepted', result.returncode == 0, result.stderr)
        return [entry['presentationId'] for entry in current.values()]

    def drawing(ink, drawing_source=None, manifest=artifact):
        transition = {'effect': 'drawing', 'duration_ms': args.duration_ms, 'from': str(args.from_image),
                      'drawing': {'artifact': str(manifest), 'ink': ink}}
        if drawing_source:
            transition['drawing_source'] = str(drawing_source)
        return transition

    def play(label, outputs, media, transition):
        apply(outputs, media, transition)
        wait(lambda: len(preludes()) == len(outputs), f'{label} drawing preludes', 15)
        started = time.monotonic()
        argv = preludes()
        report[f'{label}-preludes'] = argv
        check(f'{label}: one drawing prelude per visible output',
              all(arg[arg.index('--shader') + 1] == 'drawing' for arg in argv), argv)
        frames = []
        finished = None
        while finished is None or time.monotonic() - finished < CAPTURE_TAIL_SECONDS:
            elapsed = time.monotonic() - started
            if elapsed > PRELUDE_LIMIT_SECONDS:
                raise RuntimeError(f'{label}: drawing did not finish')
            if finished is None and not preludes():
                finished = time.monotonic()
                report[f'{label}-handoff'] = round(finished - started, 3)
            for output in outputs:
                path = capture(f'{label}-{elapsed:06.3f}', output)
                frames.append({'output': output, 'elapsed': round(elapsed, 3), 'path': str(path),
                               'presenter': finished is not None})
        report['captures'].extend(frames)
        return frames

    def stroked(frames, output):
        paper_tone = (241, 238, 229)
        hits = []
        for frame in frames:
            if frame['output'] != output:
                continue
            image = Image.open(frame['path']).convert('RGB').resize((160, 90))
            raw = image.tobytes()
            pixels = [raw[index:index + 3] for index in range(0, len(raw), 3)]
            paper_share = sum(max(abs(a - b) for a, b in zip(pixel, paper_tone)) <= PAPER_TOLERANCE
                              for pixel in pixels) / len(pixels)
            hits.append((frame['elapsed'], round(paper_share, 3)))
        return hits

    try:
        flags = run(['pkg-config', '--cflags', '--libs', 'Qt6Core', 'Qt6Gui', 'Qt6DBus']).stdout.split()
        run(['c++', '-std=c++17', '-O2', '-fPIC', str(source / 'scripts/tests/activity-capture.cpp'),
             '-o', str(root / 'capture'), *flags], timeout=60)
        video = root / 'moving.mp4'
        frames_dir = root / 'video-frames'
        frames_dir.mkdir()
        base = Image.open(reference).convert('RGB').resize((640, 360))
        for index in range(VIDEO_FRAMES):
            frame = base.copy()
            left = index * 13 % 600
            ImageDraw.Draw(frame).rectangle((left, 160, left + 39, 160 + int(360 * BAR_HEIGHT)), fill='#ffffff')
            frame.save(frames_dir / f'{index:03d}.png')
        run(['ffmpeg', '-v', 'error', '-framerate', str(VIDEO_RATE), '-i', str(frames_dir / '%03d.png'),
             '-c:v', 'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', str(video)], timeout=60)
        dbus = start(['dbus-daemon', '--session', '--nofork', '--print-address=1'], 'dbus', subprocess.PIPE)
        env['DBUS_SESSION_BUS_ADDRESS'] = dbus.stdout.readline().decode().strip()
        kwin = start(['kwin_wayland', '--virtual', '--output-count', '2', '--socket', SOCKET,
                      '--width', str(width), '--height', str(height), '--no-lockscreen',
                      '--no-global-shortcuts'], 'kwin')
        wait(lambda: (runtime / SOCKET).exists() and kwin.poll() is None, 'private KWin socket')
        env['WAYLAND_DISPLAY'] = SOCKET
        start(['/usr/lib/kactivitymanagerd'], 'activity')
        plasma = start(['plasmashell'], 'plasmashell')
        wait(lambda: script('print(desktops().length)').stdout.strip() == '2' and plasma.poll() is None,
             'private Plasma desktops', 40)
        script('panels().forEach(function(p){p.remove();});')
        outputs = [entry['name'] for entry in json.loads(run(['kscreen-doctor', '-j']).stdout)['outputs']
                   if entry['enabled']]
        report['outputs'] = outputs
        check('two private outputs', len(outputs) == 2, outputs)
        still = {'kind': 'static', 'path': str(args.from_image)}
        apply(outputs, still)
        time.sleep(SETTLE_SECONDS)
        check('candidate plugin loaded', str(plugin) in Path(f'/proc/{plasma.pid}/maps').read_text())
        target = {'kind': 'static', 'path': str(reference)}
        for ink in ['plain', 'rainbow']:
            frames = play(ink, outputs, target, drawing(ink))
            for output in outputs:
                shares = stroked(frames, output)
                report[f'{ink}-{output}-paper-share'] = shares
                check(f'{ink}: {output} shows the paper sheet while drawing',
                      any(share > 0.3 for _, share in shares), shares)
                final = Image.open([frame for frame in frames if frame['output'] == output][-1]['path'])
                expected = Image.open(reference).convert('RGB').resize(final.size)
                difference = sum(ImageStat.Stat(ImageChops.difference(final.convert('RGB'), expected)).mean) / 3
                check(f'{ink}: {output} ends on the wallpaper', difference < 12, difference)
            apply(outputs, still)
            time.sleep(SETTLE_SECONDS)
        media = {'kind': 'video', 'path': str(video), 'engine': 'default'}
        frames = play('video', outputs, media, drawing('plain', reference))
        shares = stroked(frames, outputs[0])
        report['video-paper-share'] = shares
        check('video: drawing plays before playback', any(share > 0.3 for _, share in shares), shares)
        box = None
        moving = []
        for index in range(3):
            path = capture(f'video-playing-{index}', outputs[0])
            image = Image.open(path).convert('RGB')
            box = box or (0, int(image.height * 0.4), image.width, int(image.height * 0.6))
            moving.append(image.crop(box))
            time.sleep(0.25)
        deltas = [sum(ImageStat.Stat(ImageChops.difference(moving[0], other)).mean) / 3 for other in moving[1:]]
        check('video: playback continues after the drawing', max(deltas) > MOVING_DELTA, deltas)
        log = (root / 'plasmashell.log').read_text(errors='replace')
        failures = [line for line in log.splitlines() if 'prelude' in line or 'drawing' in line]
        check('no prelude failures', not failures, failures)
        identifiers = apply(outputs, target, drawing('plain', manifest=root / 'pruned/manifest.json'))
        started = time.monotonic()
        wait(lambda: presented(identifiers) == ['ready'] * len(outputs), 'pruned drawing presentation', 15)
        report['pruned-ready-seconds'] = round(time.monotonic() - started, 3)
        frame = Image.open(capture('pruned', outputs[0])).convert('RGB')
        expected = Image.open(reference).convert('RGB').resize(frame.size)
        difference = sum(ImageStat.Stat(ImageChops.difference(frame, expected)).mean) / 3
        check('pruned artifact presents the wallpaper', difference < 12, report['pruned-ready-seconds'])
        identifiers = apply(outputs, target, drawing('rainbow'))
        wait(lambda: presented(identifiers) == ['ready'] * len(outputs), 'stored drawing presentation', 30)
        for identifier in identifiers:
            (runtime / 'skwd-paper-plasma' / f'{identifier}.json').unlink()
        plasma_log = (root / 'plasmashell.log').read_bytes()
        helpers.stop_processes([plasma])
        processes.remove(plasma)
        (root / 'plasmashell-first.log').write_bytes(plasma_log)
        plasma = start(['plasmashell'], 'plasmashell')
        wait(lambda: script('print(desktops().length)').stdout.strip() == '2' and plasma.poll() is None,
             'restarted Plasma desktops', 40)
        replayed = []
        deadline = time.monotonic() + RESTART_WATCH_SECONDS
        while time.monotonic() < deadline:
            replayed.extend(preludes())
            time.sleep(0.1)
        check('restart does not replay the stored drawing', not replayed, replayed)
        for output in outputs:
            frame = Image.open(capture(f'restarted-{output}', output)).convert('RGB')
            expected = Image.open(reference).convert('RGB').resize(frame.size)
            difference = sum(ImageStat.Stat(ImageChops.difference(frame, expected)).mean) / 3
            check(f'restart: {output} shows the wallpaper directly', difference < 12, difference)
        report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = repr(error)
        print(report['error'], flush=True)
    finally:
        helpers.stop_processes(processes)
        (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        shutil.rmtree(runtime)
    return 0 if report.get('passed') else 1


if __name__ == '__main__':
    raise SystemExit(main())
