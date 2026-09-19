#!/usr/bin/env python3
"""Check fixture pixels, playback, sharing and connector changes in private KWin.

Requires Pillow, FFmpeg, Qt Test, D-Bus, KWin and actual Vulkan/GL external-memory
support. No compositor or application in the existing session is reconfigured.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

from PIL import Image, ImageDraw

spec = importlib.util.spec_from_file_location('audio_live', Path(__file__).with_name('test-audio-live.py'))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--paper', required=True, type=Path)
    parser.add_argument('--results', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--plugin', type=Path, help='override the plugin for baseline red checks')
    parser.add_argument('--monitor-only', action='store_true')
    args = parser.parse_args()
    build, paper = args.build.resolve(strict=True), args.paper.resolve(strict=True)
    plugin_source = (args.plugin or build / 'libskwdwallpaperplugin.so').resolve(strict=True)
    root, runtime = helpers.create_directories(args.results, args.runtime)
    module = root / 'qml/org/skwd/wallpaper'
    module.mkdir(parents=True)
    plugin = module / 'libskwdwallpaperplugin.so'
    shutil.copy2(plugin_source, plugin)
    shutil.copy2(Path(__file__).resolve().parents[1] / 'qmldir', module / 'qmldir')
    for name in ['config', 'cache', 'data', 'state', 'tmp']:
        (root / name).mkdir()
    env = dict(os.environ)
    for name in ['DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XAUTHORITY',
                 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR', 'PULSE_SINK', 'PULSE_SOURCE']:
        env.pop(name, None)
    env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
               XDG_STATE_HOME=str(root / 'state'), TMPDIR=str(root / 'tmp'),
               QML_IMPORT_PATH=str(root / 'qml'), QML2_IMPORT_PATH=str(root / 'qml'),
               XDG_CURRENT_DESKTOP='KDE', XDG_SESSION_TYPE='wayland',
               QSG_RHI_BACKEND='opengl', QT_FORCE_STDERR_LOGGING='1',
               SKWD_WALL_LOG='info', SKWD_VK_DECODE='sw', GIO_USE_VFS='local',
               PULSE_SERVER=f'unix:{runtime}/unused-pulse',
               SKWD_WALL_VK_BIN=str(paper.parent / 'skwd-wall-vk'))
    processes = []
    report = {'checks': [], 'artifacts': {}}
    for artifact in [plugin, paper, paper.parent / 'skwd-wall-vk', build / 'live-monitor-test', build / 'gpu-presentation-test']:
        report['artifacts'][str(artifact)] = hashlib.sha256(artifact.read_bytes()).hexdigest()

    def start(command, name, stdout=None):
        log = open(root / f'{name}.log', 'wb')
        process = subprocess.Popen(command, env=env, stdout=stdout or log, stderr=log, start_new_session=True)
        processes.append(process)
        return process

    def checked(command, name, timeout=90):
        process = start(command, name)
        code = process.wait(timeout=timeout)
        report['checks'].append({'name': name, 'exit': code})
        print(f'{name}: exit {code}', flush=True)
        if code:
            raise RuntimeError(f'{name} failed with exit {code}; see {root / (name + ".log")}')

    try:
        for color in ['red', 'blue', 'green']:
            Image.new('RGB', (320, 180), {'red': '#ff0000', 'blue': '#0000ff', 'green': '#00ff00'}[color]).save(root / f'{color}.png')
        if not args.monitor_only:
            frames = root / 'frames'
            frames.mkdir()
            for index in range(40):
                # Grayscale isolates bridge transport from YUV chroma conversion.
                # CPU presentation separately checks exact primary colors.
                frame = Image.new('RGB', (320, 180), '#ffffff')
                draw = ImageDraw.Draw(frame)
                draw.rectangle((160, 0, 319, 179), fill='#000000')
                draw.rectangle((0, 0, 319, 59), fill='#606060')
                left = index * 7 % 280
                draw.rectangle((left, 10, left + 39, 49), fill='#ffffff')
                frame.save(frames / f'{index:03d}.png')
            checked(['ffmpeg', '-v', 'error', '-framerate', '20', '-i', str(frames / '%03d.png'),
                     '-vf', 'scale=out_color_matrix=bt709', '-colorspace', 'bt709', '-color_primaries', 'bt709',
                     '-color_trc', 'bt709', '-color_range', 'tv', '-c:v', 'libx264', '-preset', 'ultrafast',
                     '-pix_fmt', 'yuv420p', str(root / 'moving.mp4')], 'fixture')
            assignments = [{'outputs': ['*'], 'source': {'kind': 'video', 'path': str(root / 'moving.mp4'), 'engine': 'default'},
                            'mute': True, 'volume': 80, 'fill_mode': 'fill', 'layer': 'background'}]
            (root / 'assignments.json').write_text(json.dumps(assignments))
            (root / 'expectations.json').write_text(json.dumps([{'animated': True, 'pixels': [
                {'x': 0.25, 'y': 0.75, 'color': '#ffffff'}, {'x': 0.75, 'y': 0.75, 'color': '#000000'}]}]))
            report['artifacts'][str(root / 'moving.mp4')] = hashlib.sha256((root / 'moving.mp4').read_bytes()).hexdigest()
        dbus = start(['dbus-daemon', '--session', '--nofork', '--print-address=1'], 'dbus', subprocess.PIPE)
        env['DBUS_SESSION_BUS_ADDRESS'] = dbus.stdout.readline().decode().strip()
        kwin = start(['kwin_wayland', '--virtual', '--output-count', '1', '--socket', 'presentation-test',
                      '--width', '960', '--height', '1200', '--no-lockscreen', '--no-global-shortcuts'], 'kwin')
        deadline = time.monotonic() + 20
        while not (runtime / 'presentation-test').exists():
            if kwin.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError('private KWin did not become ready')
            time.sleep(0.1)
        env['WAYLAND_DISPLAY'] = 'presentation-test'
        checked([str(build / 'live-monitor-test'), str(paper), str(root), str(plugin)], 'monitor')
        if not args.monitor_only:
            env['SKWD_TEST_EXPECTATIONS_JSON'] = str(root / 'expectations.json')
            for count in [1, 2]:
                env['SKWD_TEST_SHARED_OUTPUTS'] = str(count)
                checked([str(build / 'gpu-presentation-test'), str(paper), str(root / 'assignments.json'),
                         str(root / f'gpu-{count}')], f'gpu-{count}')
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
