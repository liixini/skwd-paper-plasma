#!/usr/bin/env python3
"""Measure Plasma audio controls in a private KWin and PipeWire session."""
import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import time


def create_directories(results, runtime):
    results, runtime = results.resolve(), runtime.resolve()
    if results == runtime or results in runtime.parents or runtime in results.parents:
        raise ValueError('results and runtime must be separate non-overlapping directories')
    if len(str(runtime)) >= 65:
        raise ValueError('use a short disk-backed runtime directory')
    if results.exists() or runtime.exists():
        raise ValueError('results and runtime must both be new directories')
    results.mkdir(parents=True)
    runtime.mkdir(parents=True, mode=0o700)
    return results, runtime


def stop_processes(processes):
    for process in reversed(processes):
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    for process in reversed(processes):
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        # Children can survive their session leader; always finish the owned group.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plugin', required=True, type=Path)
    parser.add_argument('--paper', required=True, type=Path)
    parser.add_argument('--results', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--scene', type=Path, help='scene with a steady looping tone for gain-ratio checks')
    parser.add_argument('--only-lone', action='store_true', help='exercise only in-place controls')
    parser.add_argument('--only-scene', action='store_true', help='exercise only the scene fixture')
    args = parser.parse_args()
    args.plugin = args.plugin.resolve(strict=True)
    args.paper = args.paper.resolve(strict=True)
    root, runtime = create_directories(args.results, args.runtime)
    source = Path(__file__).resolve().parents[1]
    qml = root / 'qml/org/skwd/wallpaper'
    qml.mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.plugin, qml / args.plugin.name)
    shutil.copy2(source / 'qmldir', qml / 'qmldir')
    plugin = qml / args.plugin.name
    data = root / 'data'
    wallpaper = data / 'plasma/wallpapers/org.skwd.wall.plasma'
    shutil.copytree(source / 'wallpaper', wallpaper)
    for name in ['config', 'cache', 'state', 'tmp']:
        (root / name).mkdir(exist_ok=True)
    config = root / 'config'
    wireplumber = config / 'wireplumber/wireplumber.conf.d'
    wireplumber.mkdir(parents=True, exist_ok=True)
    (wireplumber / 'disable-hardware.conf').write_text('''wireplumber.profiles = {
    main = {
        monitor.alsa = disabled
        monitor.bluez = disabled
        monitor.bluez-midi = disabled
        monitor.v4l2 = disabled
        monitor.libcamera = disabled
    }
}
''')
    alsa = root / 'alsa.conf'
    alsa.write_text('</usr/share/alsa/alsa.conf>\npcm.!default { type pulse }\nctl.!default { type pulse }\n')
    env = dict(os.environ)
    for name in ['DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XAUTHORITY',
                 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR', 'PULSE_SINK', 'PULSE_SOURCE']:
        env.pop(name, None)
    env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(config),
               XDG_DATA_HOME=str(data), XDG_CACHE_HOME=str(root / 'cache'),
               XDG_STATE_HOME=str(root / 'state'), TMPDIR=str(root / 'tmp'),
               XDG_DATA_DIRS=f'{data}:/usr/local/share:/usr/share',
               QML_IMPORT_PATH=str(root / 'qml'), QML2_IMPORT_PATH=str(root / 'qml'),
               XDG_CURRENT_DESKTOP='KDE', XDG_SESSION_DESKTOP='KDE',
               XDG_SESSION_TYPE='wayland', KDE_FULL_SESSION='true', KDE_SESSION_VERSION='6',
               QSG_RHI_BACKEND='opengl', QT_FORCE_STDERR_LOGGING='1', SKWD_VK_DECODE='sw',
               SKWD_WALL_LOG='info',
               PULSE_SERVER=f'unix:{runtime}/pulse/native', ALSA_CONFIG_PATH=str(alsa),
               SKWD_WALL_VK_BIN=str(args.paper.parent / 'skwd-wall-vk'))
    processes = []
    report = {'checks': [], 'samples': {}, 'artifacts': {}, 'running_artifacts': {}}
    for artifact in [args.paper, args.paper.parent / 'skwd-wall-vk', plugin]:
        report['artifacts'][str(artifact)] = hashlib.sha256(artifact.read_bytes()).hexdigest()

    def run(command, check=True, timeout=15):
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

    def wait(predicate, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.1)
        raise RuntimeError('condition timed out')

    def check(name, ok, detail=None):
        report['checks'].append({'name': name, 'passed': bool(ok), 'detail': detail})
        print(json.dumps(report['checks'][-1]), flush=True)
        if not ok:
            raise AssertionError(name)

    def qdbus(script):
        return run(['qdbus6', 'org.kde.plasmashell', '/PlasmaShell',
                    'org.kde.PlasmaShell.evaluateScript', script], check=False)

    def renderers():
        found = []
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                process_env = (entry / 'environ').read_bytes().split(b'\0')
                if f'XDG_RUNTIME_DIR={runtime}'.encode() not in process_env:
                    continue
                executable = (entry / 'exe').resolve()
                if executable.name == 'skwd-wall-vk':
                    found.append(int(entry.name))
            except (OSError, PermissionError):
                pass
        return sorted(found)

    def sample(label):
        time.sleep(1.2)
        path = root / f'{label}.f32le'
        with path.open('wb') as capture:
            recorder = subprocess.Popen(['parec', '--device=skwd-test.monitor', '--format=float32le',
                                         '--rate=48000', '--channels=2'], env=env, stdout=capture,
                                        stderr=subprocess.PIPE, start_new_session=True)
            processes.append(recorder)
            try:
                time.sleep(2.5)
            finally:
                stop_processes([recorder])
                recorder.communicate()
                processes.remove(recorder)
        values = array.array('f')
        values.frombytes(path.read_bytes())
        check(f'{label}: captured samples', len(values) > 48000, len(values))
        # Discard capture startup and retain the last second.
        values = values[-96000:]
        rms = math.sqrt(sum(value * value for value in values) / len(values))
        pids = renderers()
        for pid in pids:
            if str(pid) not in report['running_artifacts']:
                executable = Path(f'/proc/{pid}/exe')
                digest = hashlib.sha256(executable.read_bytes()).hexdigest()
                report['running_artifacts'][str(pid)] = {'path': str(executable.resolve()), 'sha256': digest}
                check(f'{label}: running renderer matches staged binary',
                      digest == report['artifacts'][str(args.paper.parent / 'skwd-wall-vk')],
                      report['running_artifacts'][str(pid)])
        report['samples'][label] = {'rms': rms, 'pids': pids, 'frames': len(values) // 2}
        (root / f'{label}-sink-inputs.json').write_text(run(['pactl', '-f', 'json', 'list', 'sink-inputs']).stdout)
        print(f'{label}: RMS={rms:.8f} pids={renderers()}', flush=True)
        return rms

    try:
        video = root / 'tone.mp4'
        still = root / 'still.png'
        run(['ffmpeg', '-y', '-v', 'error', '-f', 'lavfi', '-i', 'color=blue:size=320x180',
             '-frames:v', '1', str(still)])
        run(['ffmpeg', '-y', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=20',
             '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000', '-t', '6', '-c:v',
             'libx264', '-preset', 'ultrafast', '-pix_fmt', 'yuv420p', '-c:a', 'aac', str(video)], timeout=40)
        scene = args.scene.resolve() if args.scene else root / 'scene'
        if not args.scene:
            scene.mkdir(exist_ok=True)
            tone = root / 'tone.m4a'
            run(['ffmpeg', '-y', '-v', 'error', '-f', 'lavfi', '-i',
                 'sine=frequency=440:sample_rate=48000', '-t', '6', '-c:a', 'aac', str(tone)])
            document = {'general': {'orthogonalprojection': {'width': 640, 'height': 360},
                         'clearcolor': '0 0 0', 'cameraparallax': False},
                        'objects': [{'id': 1, 'name': 'Green fixture', 'image': 'models/test.json',
                          'size': '640 360', 'origin': '320 180 0', 'scale': '1 1 1',
                          'angles': '0 0 0', 'color': '1 1 1', 'alpha': 1},
                          {'id': 2, 'name': 'Tone', 'sound': ['sounds/tone.m4a'],
                           'volume': 1, 'playbackmode': 'loop', 'visible': True}]}
            model = {'material': 'materials/test.json', 'width': 640, 'height': 360}
            material = {'passes': [{'shader': 'genericimage2', 'textures': ['test'],
                         'blending': 'normal', 'cullmode': 'nocull', 'depthtest': 'disabled',
                         'depthwrite': 'disabled'}]}
            texture = b'TEXV0005\0TEXI0001\0' + struct.pack('<7i', 0, 0, 4, 4, 4, 4, 0)
            texture += b'TEXB0001\0' + struct.pack('<5i', 1, 1, 4, 4, 64) + bytes([0, 255, 0, 255]) * 16
            files = {'scene.json': json.dumps(document).encode(), 'sounds/tone.m4a': tone.read_bytes(),
                     'models/test.json': json.dumps(model).encode(),
                     'materials/test.json': json.dumps(material).encode(), 'materials/test.tex': texture}
            table = struct.pack('<I', 8) + b'PKGV0007' + struct.pack('<I', len(files))
            payload = b''
            for name, contents in files.items():
                encoded = name.encode()
                table += struct.pack('<I', len(encoded)) + encoded + struct.pack('<II', len(payload), len(contents))
                payload += contents
            (scene / 'scene.pkg').write_bytes(table + payload)
            (scene / 'project.json').write_text(json.dumps({'type': 'scene', 'file': 'scene.json', 'title': 'Audio regression fixture'}))
        report['artifacts'][str(scene / 'scene.pkg')] = hashlib.sha256((scene / 'scene.pkg').read_bytes()).hexdigest()
        dbus = start(['dbus-daemon', '--session', '--nofork', '--print-address=1'], 'dbus', subprocess.PIPE)
        env['DBUS_SESSION_BUS_ADDRESS'] = dbus.stdout.readline().decode().strip()
        start(['pipewire'], 'pipewire')
        wait(lambda: (runtime / 'pipewire-0').exists())
        start(['pipewire-pulse'], 'pipewire-pulse')
        start(['wireplumber'], 'wireplumber')
        wait(lambda: run(['pactl', 'info'], check=False).returncode == 0)
        run(['pactl', 'load-module', 'module-null-sink', 'sink_name=skwd-test',
             'sink_properties=device.description=SKWDPrivateTest'])
        run(['pactl', 'set-default-sink', 'skwd-test'])
        env['PULSE_SINK'] = 'skwd-test'
        check('only private null audio sink exists', len(json.loads(run(['pactl', '-f', 'json', 'list', 'sinks']).stdout)) == 1)
        kwin = start(['kwin_wayland', '--virtual', '--output-count', '2', '--socket', 'audio-test',
                      '--width', '640', '--height', '360', '--no-lockscreen', '--no-global-shortcuts'], 'kwin')
        wait(lambda: (runtime / 'audio-test').exists() and kwin.poll() is None)
        env['WAYLAND_DISPLAY'] = 'audio-test'
        activity = start(['/usr/lib/kactivitymanagerd'], 'activity')
        def activity_ready():
            result = run(['qdbus6', 'org.kde.ActivityManager', '/ActivityManager/Activities',
                          'org.kde.ActivityManager.Activities.CurrentActivity'], check=False)
            return result.returncode == 0 and bool(result.stdout.strip()) and activity.poll() is None
        wait(activity_ready)
        plasma = start(['plasmashell'], 'plasmashell')
        def desktop_ready():
            result = qdbus('print(desktops().length)')
            return result.returncode == 0 and result.stdout.strip() == '2' and plasma.poll() is None
        wait(desktop_ready, 40)
        output_info = json.loads(run(['kscreen-doctor', '-j']).stdout)
        outputs = [entry['name'] for entry in output_info['outputs'] if entry['enabled']]
        report['outputs'] = outputs
        check('two private KWin outputs', len(outputs) == 2, output_info)
        sources = [('video', {'kind': 'video', 'path': str(video), 'engine': 'default'})]
        sources.append(('scene', {'kind': 'we', 'path': str(scene)}))
        if args.only_scene:
            sources = sources[1:]
        for kind, media in sources:
            current = {name: {'assignment': {'outputs': [name], 'source': media, 'mute': False,
                          'volume': 100, 'fill_mode': 'fill', 'layer': 'background'},
                          'paper': str(args.paper), 'width': 320, 'height': 180, 'fps': 20,
                          'paused': False, 'manualPaused': False} for name in outputs}

            def apply():
                encoded = json.dumps(current, separators=(',', ':'))
                script = ('var a=' + encoded + ';var encoded=JSON.stringify(a);desktops().forEach(function(d){'
                          'd.currentConfigGroup=["Wallpaper","org.skwd.wall.plasma","General"];'
                          'd.writeConfig("Assignments",encoded);d.wallpaperPlugin="org.skwd.wall.plasma";});')
                result = qdbus(script)
                check(f'{kind}: assignment accepted', result.returncode == 0, result.stderr)

            apply()
            wait(lambda: bool(renderers()), 25)
            time.sleep(3)
            check(f'{kind}: candidate plugin loaded', str(plugin) in Path(f'/proc/{plasma.pid}/maps').read_text())
            if not args.only_lone:
                high = sample(f'{kind}-shared-100')
                check(f'{kind}: audible baseline', high > 0.005, high)
                check(f'{kind}: initial shared renderer', len(renderers()) == 1, renderers())
                current[outputs[0]]['assignment']['mute'] = True
                apply()
                single = sample(f'{kind}-one-muted')
                check(f'{kind}: other output remains audible', 0.75 < single / high < 1.25, single / high)
                check(f'{kind}: differing audio separates renderers', len(renderers()) == 2, renderers())
                current[outputs[1]]['assignment']['mute'] = True
                apply()
                silent = sample(f'{kind}-both-muted')
                check(f'{kind}: both muted', silent < high * 0.001, silent)
                check(f'{kind}: matching audio shares renderer again', len(renderers()) == 1, renderers())
                for name in outputs:
                    current[name]['assignment']['mute'] = False
                    current[name]['assignment']['volume'] = 25
                apply()
                low = sample(f'{kind}-shared-25')
                check(f'{kind}: quarter volume', 0.20 < low / high < 0.30, low / high)
                for name in outputs:
                    current[name]['assignment']['volume'] = 100
                apply()
                restored = sample(f'{kind}-restored-100')
                check(f'{kind}: restored volume', 0.85 < restored / high < 1.15, restored / high)
                for name in outputs:
                    current[name]['assignment'].pop('mute')
                    current[name]['assignment'].pop('volume')
                apply()
                defaults = sample(f'{kind}-omitted-defaults')
                check(f'{kind}: omitted mute defaults to silence', defaults < high * 0.001, defaults)
                for name in outputs:
                    current[name]['assignment']['mute'] = False
                apply()
                default_volume = sample(f'{kind}-default-80')
                check(f'{kind}: omitted volume defaults to 80', 0.72 < default_volume / high < 0.88,
                      default_volume / high)
            current[outputs[1]]['assignment']['source'] = {'kind': 'static', 'path': str(still)}
            current[outputs[0]]['assignment'].update(mute=False, volume=100)
            apply()
            lone = sample(f'{kind}-lone-100')
            baseline_pids = renderers()
            check(f'{kind}: lone audible renderer', len(baseline_pids) == 1 and lone > 0.005,
                  {'pids': baseline_pids, 'rms': lone})
            updates = [('lone-muted', {'mute': True, 'volume': 100}, 0.0),
                       ('lone-unmuted', {'mute': False, 'volume': 100}, 1.0),
                       ('lone-25', {'mute': False, 'volume': 25}, 0.25),
                       ('lone-restored', {'mute': False, 'volume': 100}, 1.0),
                       ('lone-defaults', {}, 0.0),
                       ('lone-default-volume', {'mute': False}, 0.8)]
            for label, settings, expected in updates:
                current[outputs[0]]['assignment'].pop('mute', None)
                current[outputs[0]]['assignment'].pop('volume', None)
                current[outputs[0]]['assignment'].update(settings)
                apply()
                measured = sample(f'{kind}-{label}')
                check(f'{kind}: {label} keeps renderer alive', renderers() == baseline_pids,
                      {'before': baseline_pids, 'after': renderers()})
                ratio = measured / lone
                tolerance = 0.001 if expected == 0 else 0.06
                check(f'{kind}: {label} measured audio', abs(ratio - expected) < tolerance,
                      {'expected': expected, 'actual': ratio})
        report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = repr(error)
        print(report['error'], flush=True)
    finally:
        stop_processes(processes)
        (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        # Sockets belong only to the private processes that have now exited.
        shutil.rmtree(runtime)
    return 0 if report.get('passed') else 1


if __name__ == '__main__':
    raise SystemExit(main())
