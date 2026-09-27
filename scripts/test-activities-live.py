#!/usr/bin/env python3
"""Verify shared wallpaper rendering across real Plasma activities in private KWin.

Uses a separate session bus, activity manager, compositor, configuration and data
directory. Captures composed output pixels and retains all logs and process
arguments. Requires Pillow, FFmpeg, a C++ compiler, Qt DBus, KWin, Plasma and GPU access.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time

from PIL import Image, ImageChops, ImageDraw, ImageStat

spec = importlib.util.spec_from_file_location('audio_live', Path(__file__).with_name('test-audio-live.py'))
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plugin', required=True, type=Path)
    parser.add_argument('--paper', required=True, type=Path)
    parser.add_argument('--results', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--outputs', type=int, choices=[1, 2], default=2)
    parser.add_argument('--activities', type=int, choices=[2, 3], default=3)
    parser.add_argument('--cycles', type=int, default=3)
    parser.add_argument('--kind', choices=['both', 'static', 'video', 'scene'], default='both')
    args = parser.parse_args()
    plugin_source, paper = args.plugin.resolve(strict=True), args.paper.resolve(strict=True)
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
    # Keep the cursor outside the pixel-check regions and disable decorative
    # activity-change animations so captures measure the wallpaper itself.
    (root / 'config/kwinrc').write_text('[Plugins]\nslideEnabled=false\nslidebackEnabled=false\n')
    wrapper = root / 'paper-wrapper.py'
    wrapper.write_text('#!/usr/bin/python3\nimport json, os, sys\n'
                       f'with open({str(root / "spawns.jsonl")!r}, "a") as out:\n'
                       '    out.write(json.dumps(sys.argv[1:]) + "\\n")\n'
                       f'os.execv({str(paper)!r}, [{str(paper)!r}, *sys.argv[1:]])\n')
    wrapper.chmod(0o755)
    env = dict(os.environ)
    for name in ['DISPLAY', 'WAYLAND_DISPLAY', 'DBUS_SESSION_BUS_ADDRESS', 'XAUTHORITY',
                 'PIPEWIRE_REMOTE', 'PIPEWIRE_RUNTIME_DIR', 'PULSE_SINK', 'PULSE_SOURCE']:
        env.pop(name, None)
    env.update(XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / 'config'),
               XDG_DATA_HOME=str(data), XDG_CACHE_HOME=str(root / 'cache'),
               XDG_STATE_HOME=str(root / 'state'), TMPDIR=str(root / 'tmp'),
               XDG_DATA_DIRS=f'{data}:/usr/local/share:/usr/share',
               QML_IMPORT_PATH=str(root / 'qml'), QML2_IMPORT_PATH=str(root / 'qml'),
               XDG_CURRENT_DESKTOP='KDE', XDG_SESSION_DESKTOP='KDE',
               XDG_SESSION_TYPE='wayland', KDE_FULL_SESSION='true', KDE_SESSION_VERSION='6',
               QSG_RHI_BACKEND='opengl', QT_QPA_PLATFORM='wayland', QT_FORCE_STDERR_LOGGING='1',
               KWIN_SCREENSHOT_NO_PERMISSION_CHECKS='1', KWIN_WAYLAND_NO_PERMISSION_CHECKS='1',
               SKWD_VK_DECODE='sw', SKWD_WALL_LOG='info', GIO_USE_VFS='local',
               PULSE_SERVER=f'unix:{runtime}/unused-pulse',
               SKWD_WALL_VK_BIN=str(paper.parent / 'skwd-wall-vk'),
               SKWD_WALL_STILL_BIN=str(paper.parent / 'skwd-wall-still'))
    processes = []
    report = {'checks': [], 'snapshots': [], 'artifacts': {}, 'running_artifacts': {},
              'requested': {'outputs': args.outputs, 'activities': args.activities, 'cycles': args.cycles}}
    for artifact in [paper, paper.parent / 'skwd-wall-vk', paper.parent / 'skwd-wall-still', plugin,
                     Path(__file__).resolve(), source / 'scripts/tests/activity-capture.cpp',
                     source / 'wallpaper/contents/ui/main.qml']:
        report['artifacts'][str(artifact)] = hashlib.sha256(artifact.read_bytes()).hexdigest()

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

    def activity(method, *values):
        return run(['qdbus6', 'org.kde.ActivityManager', '/ActivityManager/Activities',
                    'org.kde.ActivityManager.Activities.' + method, *values])

    def renderers():
        found = []
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                if f'XDG_RUNTIME_DIR={runtime}'.encode() not in (entry / 'environ').read_bytes().split(b'\0'):
                    continue
                executable = (entry / 'exe').resolve()
                if executable.name not in [paper.name, 'skwd-wall-vk', 'skwd-wall-still']:
                    continue
                # A transition may briefly fork one prelude for each visible
                # stream. Track the one shared presenter that Plasma owns; it
                # later execs the final renderer while preserving its PID.
                stat = (entry / 'stat').read_text()
                if int(stat[stat.rfind(')') + 2:].split()[1]) != plasma.pid:
                    continue
                pid = int(entry.name)
                argv = (entry / 'cmdline').read_bytes().decode().strip('\0').split('\0')
                found.append({'pid': pid, 'argv': argv, 'executable': str(executable)})
                artifact_key = f'{pid}:{executable.name}'
                if artifact_key not in report['running_artifacts']:
                    digest = hashlib.sha256((entry / 'exe').read_bytes()).hexdigest()
                    report['running_artifacts'][artifact_key] = {'path': str(executable), 'sha256': digest}
                    check('running renderer matches staged artifact',
                          digest == report['artifacts'][str(paper.parent / executable.name)],
                          report['running_artifacts'][artifact_key])
            except (OSError, PermissionError):
                pass
        return sorted(found, key=lambda row: row['pid'])

    def capture(label, output):
        path = root / 'captures' / f'{label}-{output}.png'
        run([str(root / 'activity-capture'), output, str(path)])
        return Image.open(path).convert('RGB')

    def verify(label, kind, outputs, stable_pids=None):
        time.sleep(0.65)
        running = renderers()
        report['snapshots'].append({'label': label, 'renderers': running})
        if len(running) != 1:
            for output in outputs:
                capture(f'{label}-failure', output)
        check(f'{label}: exactly one shared presenter', len(running) == 1, running)
        pids = [row['pid'] for row in running]
        if stable_pids is not None:
            check(f'{label}: activity switch preserves renderer', pids == stable_pids, pids)
        for output in outputs:
            frames = []
            for index in range(4 if kind == 'video' else 1):
                image = capture(f'{label}-{index}', output)
                frames.append(image)
                # Fixture regions avoid Plasma panels, desktop toolboxes and cursors.
                for fraction, expected in [(0.25, 255), (0.75, 0)]:
                    x, y = int(image.width * fraction), int(image.height * 0.65)
                    pixel = image.getpixel((x, y))
                    check(f'{label}: {output} fixture pixel {fraction} frame {index}',
                          max(abs(channel - expected) for channel in pixel) <= 25, pixel)
                time.sleep(0.17)
            if kind == 'video':
                # A moving bright bar occupies the middle third of the fixture.
                box = (0, int(frames[0].height * 0.22), frames[0].width, int(frames[0].height * 0.45))
                deltas = [sum(ImageStat.Stat(ImageChops.difference(frames[0].crop(box), frame.crop(box))).mean) / 3
                          for frame in frames[1:]]
                check(f'{label}: {output} video continues moving', max(deltas) > 4, deltas)
        return pids

    try:
        flags = run(['pkg-config', '--cflags', '--libs', 'Qt6Core', 'Qt6Gui', 'Qt6DBus']).stdout.split()
        run(['c++', '-std=c++17', '-O2', '-fPIC', str(source / 'scripts/tests/activity-capture.cpp'),
             '-o', str(root / 'activity-capture'), *flags], timeout=60)
        still = root / 'still.png'
        fixture = Image.new('RGB', (320, 180), '#ffffff')
        ImageDraw.Draw(fixture).rectangle((160, 0, 319, 179), fill='#000000')
        fixture.save(still)
        black = root / 'black.png'
        Image.new('RGB', (320, 180), '#000000').save(black)
        video = root / 'moving.mp4'
        if args.kind in ['both', 'video']:
            frames = root / 'frames'
            frames.mkdir()
            for index in range(60):
                frame = fixture.copy()
                draw = ImageDraw.Draw(frame)
                draw.rectangle((0, 40, 319, 80), fill='#606060')
                left = index * 7 % 280
                draw.rectangle((left, 40, left + 39, 80), fill='#ffffff')
                frame.save(frames / f'{index:03d}.png')
            run(['ffmpeg', '-v', 'error', '-framerate', '20', '-i', str(frames / '%03d.png'),
                 '-vf', 'scale=out_color_matrix=bt709', '-colorspace', 'bt709', '-color_primaries', 'bt709',
                 '-color_trc', 'bt709', '-color_range', 'tv', '-c:v', 'libx264', '-preset', 'ultrafast',
                 '-pix_fmt', 'yuv420p', str(video)], timeout=60)
        for fixture_path in [still] + ([video] if video.exists() else []):
            report['artifacts'][str(fixture_path)] = hashlib.sha256(fixture_path.read_bytes()).hexdigest()
        scene = root / 'scene'
        if args.kind == 'scene':
            scene.mkdir()
            document = {'general': {'orthogonalprojection': {'width': 640, 'height': 360},
                         'clearcolor': '0 0 0', 'cameraparallax': False},
                        'objects': [{'id': 1, 'name': 'White black fixture', 'image': 'models/test.json',
                          'size': '640 360', 'origin': '320 180 0', 'scale': '1 1 1',
                          'angles': '0 0 0', 'color': '1 1 1', 'alpha': 1}]}
            model = {'material': 'materials/test.json', 'width': 640, 'height': 360}
            material = {'passes': [{'shader': 'genericimage2', 'textures': ['test'],
                         'blending': 'normal', 'cullmode': 'nocull', 'depthtest': 'disabled',
                         'depthwrite': 'disabled'}]}
            texture = b'TEXV0005\0TEXI0001\0' + struct.pack('<7i', 0, 0, 4, 4, 4, 4, 0)
            texture += b'TEXB0001\0' + struct.pack('<5i', 1, 1, 4, 4, 64)
            texture += (bytes([255, 255, 255, 255]) * 2 + bytes([0, 0, 0, 255]) * 2) * 4
            files = {'scene.json': json.dumps(document).encode(), 'models/test.json': json.dumps(model).encode(),
                     'materials/test.json': json.dumps(material).encode(), 'materials/test.tex': texture}
            table = struct.pack('<I', 8) + b'PKGV0007' + struct.pack('<I', len(files))
            payload = b''
            for name, contents in files.items():
                encoded = name.encode()
                table += struct.pack('<I', len(encoded)) + encoded + struct.pack('<II', len(payload), len(contents))
                payload += contents
            (scene / 'scene.pkg').write_bytes(table + payload)
            (scene / 'project.json').write_text(json.dumps({'type': 'scene', 'file': 'scene.json',
                                                         'title': 'Activities native scene fixture'}))
            for fixture_path in scene.iterdir():
                report['artifacts'][str(fixture_path)] = hashlib.sha256(fixture_path.read_bytes()).hexdigest()
        dbus = start(['dbus-daemon', '--session', '--nofork', '--print-address=1'], 'dbus', subprocess.PIPE)
        env['DBUS_SESSION_BUS_ADDRESS'] = dbus.stdout.readline().decode().strip()
        kwin = start(['kwin_wayland', '--virtual', '--output-count', str(args.outputs), '--socket', 'activity-test',
                      '--width', '640', '--height', '360', '--no-lockscreen', '--no-global-shortcuts'], 'kwin')
        wait(lambda: (runtime / 'activity-test').exists() and kwin.poll() is None, 'private KWin socket')
        env['WAYLAND_DISPLAY'] = 'activity-test'
        manager = start(['/usr/lib/kactivitymanagerd'], 'activity')
        def activity_ready():
            result = run(['qdbus6', 'org.kde.ActivityManager', '/ActivityManager/Activities',
                          'org.kde.ActivityManager.Activities.CurrentActivity'], check=False)
            return result.returncode == 0 and bool(result.stdout.strip()) and manager.poll() is None
        wait(activity_ready, 'private activity manager')
        first = activity('CurrentActivity').stdout.strip()
        plasma = start(['plasmashell'], 'plasmashell')
        wait(lambda: script('print(desktops().length)').stdout.strip() == str(args.outputs)
             and plasma.poll() is None, 'private Plasma desktops', 40)
        # Widgets are irrelevant to wallpaper rendering and would contaminate pixels.
        script('panels().forEach(function(p){p.remove();});')
        output_info = json.loads(run(['kscreen-doctor', '-j']).stdout)
        outputs = [entry['name'] for entry in output_info['outputs'] if entry['enabled']]
        report['outputs'] = outputs
        check('private KWin output count', len(outputs) == args.outputs, output_info)
        activities = [first]
        report['activities'] = activities

        def verify_spawn(label, visible_outputs=None):
            argv = json.loads((root / 'spawns.jsonl').read_text().splitlines()[-1])
            assignment = json.loads(argv[argv.index('--assignment') + 1])
            streams = [dict(part.split('=', 1) for part in argv[index + 1].split(','))
                       for index, value in enumerate(argv) if value == '--stream']
            check(f'{label}: physical outputs remain unique',
                  sorted(assignment['outputs']) == sorted(outputs), assignment['outputs'])
            check(f'{label}: every activity has one stream per output',
                  len(streams) == len(activities) * len(outputs), streams)
            check(f'{label}: instance routing identities are unique',
                  len({stream['output'] for stream in streams}) == len(streams), streams)
            if len(activities) > 1:
                expected_visible = len(outputs) if visible_outputs is None else visible_outputs
                check(f'{label}: only current activity starts unpaused',
                      sum(stream['paused'] == '0' for stream in streams) == expected_visible, streams)

        def apply(activity_ids, kind, media_path=None, transition=None):
            default_path = {'static': still, 'video': video, 'scene': scene}[kind]
            media = {'kind': 'we' if kind == 'scene' else kind, 'path': str(media_path or default_path)}
            if kind == 'video':
                media['engine'] = 'default'
            current = {name: {'assignment': {'outputs': [name], 'source': media, 'mute': True,
                              'volume': 80, 'fill_mode': 'fill', 'layer': 'background'},
                              'paper': str(wrapper), 'width': 640, 'height': 360, 'fps': 20,
                              'paused': False, 'manualPaused': False} for name in outputs}
            if transition:
                for value in current.values():
                    value['assignment']['transition'] = transition
            encoded = json.dumps(current, separators=(',', ':'))
            js = ('var ids=' + json.dumps(activity_ids) + ';var a=' + encoded + ';'
                  'ids.forEach(function(id){desktopsForActivity(id).forEach(function(d){'
                  'd.currentConfigGroup=["General"];d.writeConfig("url",'
                  + json.dumps((root / 'empty-desktop').as_uri()) + ');'
                  'd.currentConfigGroup=["Wallpaper","org.skwd.wall.plasma","General"];'
                  'd.writeConfig("Assignments",JSON.stringify(a));d.wallpaperPlugin="org.skwd.wall.plasma";});});')
            result = script(js)
            check(f'{kind}: assignments accepted', result.returncode == 0, result.stderr)

        def switch(target):
            activity('SetCurrentActivity', target)
            wait(lambda: activity('CurrentActivity').stdout.strip() == target, 'activity switch')

        kinds = ['static', 'video'] if args.kind == 'both' else [args.kind]
        for kind in kinds:
            apply(activities, kind)
            wait(lambda: bool(renderers()), f'{kind} renderer start')
            time.sleep(2)
            check(f'{kind}: candidate plugin loaded', str(plugin) in Path(f'/proc/{plasma.pid}/maps').read_text())
            verify(f'{kind}-initial', kind, outputs)
            while len(activities) < args.activities:
                added = activity('AddActivity', f'SKWD regression {len(activities) + 1}').stdout.strip()
                check('activity created', bool(re.fullmatch('[0-9a-f-]{36}', added)), added)
                activities.append(added)
                switch(added)
                wait(lambda: script(f'print(desktopsForActivity({json.dumps(added)}).length)').stdout.strip()
                     == str(args.outputs), 'new activity desktops')
                apply([added], kind)
                time.sleep(2)
                verify(f'{kind}-add-{len(activities)}', kind, outputs)
            stable_pids = verify(f'{kind}-all-loaded', kind, outputs)
            verify_spawn(f'{kind}-all-loaded')
            for cycle in range(args.cycles):
                for index, target in enumerate(activities):
                    switch(target)
                    verify(f'{kind}-cycle-{cycle}-activity-{index}', kind, outputs, stable_pids)
            if kind == 'video':
                blank = activity('AddActivity', 'SKWD empty activity').stdout.strip()
                switch(blank)
                wait(lambda: script(f'print(desktopsForActivity({json.dumps(blank)}).length)').stdout.strip()
                     == str(args.outputs), 'blank activity desktops')
                result = script('desktopsForActivity(' + json.dumps(blank) + ').forEach(function(d){'
                                'd.currentConfigGroup=["General"];d.writeConfig("url",'
                                + json.dumps((root / 'empty-desktop').as_uri()) + ');'
                                'd.currentConfigGroup=["Wallpaper","org.kde.image","General"];'
                                'd.writeConfig("Image",' + json.dumps(black.as_uri()) + ');'
                                'd.wallpaperPlugin="org.kde.image";});')
                check('all-hidden: blank activity configured', result.returncode == 0, result.stderr)
                time.sleep(0.8)
                hidden_video = root / 'all-hidden.mp4'
                shutil.copy2(video, hidden_video)
                apply(activities, kind, hidden_video)
                time.sleep(2.5)
                hidden_presenters = renderers()
                check('all-hidden: one presenter starts', len(hidden_presenters) == 1, hidden_presenters)
                verify_spawn('all-hidden', visible_outputs=0)
                hidden_pid = hidden_presenters[0]['pid']
                for output in outputs:
                    frame = capture('all-hidden-blank', output)
                    pixel = frame.getpixel((frame.width // 4, int(frame.height * 0.65)))
                    check(f'all-hidden: {output} empty activity remains black', max(pixel) < 5, pixel)
                def cpu_ticks(pid):
                    stat = Path(f'/proc/{pid}/stat').read_text()
                    fields = stat[stat.rfind(')') + 2:].split()
                    return int(fields[11]) + int(fields[12])
                before = cpu_ticks(hidden_pid)
                time.sleep(3.2)
                after = cpu_ticks(hidden_pid)
                check('all-hidden: paused video consumes at most 0.05 CPU seconds',
                      (after - before) / os.sysconf('SC_CLK_TCK') <= 0.05,
                      {'before': before, 'after': after, 'ticks_per_second': os.sysconf('SC_CLK_TCK'),
                       'wall_seconds': 3.2})
                switch(activities[0])
                verify('all-hidden-resumed', kind, outputs, [hidden_pid])
                activity('RemoveActivity', blank)
                wait(lambda: blank not in activity('ListActivities').stdout, 'blank activity removal')
                for mid_switch in [False, True]:
                    label = 'transition-mid-switch' if mid_switch else 'transition-hidden-start'
                    target_video = root / f'{label}.mp4'
                    shutil.copy2(video, target_video)
                    apply(activities, kind, target_video,
                          {'duration_ms': 2400, 'effect': 'crossfade', 'from': str(black)})
                    # Observe an actual intermediate composed frame before testing
                    # either an already-hidden consumer or a hide during transition.
                    intermediate = []
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        frame = capture(f'{label}-probe-{len(intermediate)}', outputs[0])
                        color = frame.getpixel((frame.width // 4, int(frame.height * 0.65)))
                        intermediate.append(list(color))
                        if all(16 < channel < 239 for channel in color):
                            break
                        time.sleep(0.08)
                    check(f'{label}: visible intermediate transition frame',
                          bool(intermediate) and all(16 < channel < 239 for channel in intermediate[-1]),
                          intermediate)
                    transition_pids = [row['pid'] for row in renderers()]
                    verify_spawn(label)
                    preludes = []
                    for entry in Path('/proc').iterdir():
                        if not entry.name.isdigit():
                            continue
                        try:
                            stat = (entry / 'stat').read_text()
                            parent = int(stat[stat.rfind(')') + 2:].split()[1])
                            argv = (entry / 'cmdline').read_bytes().decode().strip('\0').split('\0')
                            if parent in transition_pids and '--preview-stream' in argv:
                                preludes.append({'pid': int(entry.name), 'argv': argv})
                        except OSError:
                            pass
                    check(f'{label}: only visible streams run transition preludes',
                          len(preludes) == len(outputs), preludes)
                    if mid_switch:
                        switch(activities[0] if activity('CurrentActivity').stdout.strip() != activities[0]
                               else activities[1])
                    time.sleep(2.8)
                    verify(label, kind, outputs, transition_pids)
            desktop_state = script('print(JSON.stringify(' + json.dumps(activities) + '.map(function(id){return {activity:id,desktops:desktopsForActivity(id).map(function(d){return {id:d.id,screen:d.screen,wallpaper:d.wallpaperPlugin};})};})))')
            (root / f'{kind}-desktops.json').write_text(desktop_state.stdout)
        if len(activities) == 3:
            switch(activities[0])
            removed = activities.pop()
            # Plasma can retain a removed activity's hidden graphics objects
            # after its desktop wrappers disappear. Replacing its wallpaper
            # is an explicit graphics-object destruction boundary and must
            # release those stream memberships before deleting the activity.
            result = script('desktopsForActivity(' + json.dumps(removed) + ').forEach(function(d){'
                            'd.wallpaperPlugin="org.kde.image";});')
            check('wallpaper-replaced: inactive activity accepted another plugin',
                  result.returncode == 0, result.stderr)
            def remaining_streams():
                argv = json.loads((root / 'spawns.jsonl').read_text().splitlines()[-1])
                return argv.count('--stream') == len(activities) * len(outputs)
            wait(remaining_streams, 'replaced wallpaper stream destruction')
            time.sleep(4)
            verify('wallpaper-replaced', kinds[-1], outputs)
            verify_spawn('wallpaper-replaced')
            report['removed_activity'] = removed
            activity('RemoveActivity', removed)
            wait(lambda: removed not in activity('ListActivities').stdout, 'activity removal')
            wait(lambda: script('print(desktops().length)').stdout.strip() == str(len(activities) * len(outputs)),
                 'removed activity wallpaper destruction')
            verify('activity-removed', kinds[-1], outputs)
            verify_spawn('activity-removed')
            stable_pids = [row['pid'] for row in renderers()]
            switch(activities[1])
            verify('activity-removed-switch', kinds[-1], outputs, stable_pids)
        log = (root / 'plasmashell.log').read_text(errors='replace')
        failures = [line for line in log.splitlines() if re.search(
            r'duplicate output|renderer.*(?:failed|exited)|failed to import|broken pipe|invalid.*stream', line, re.I)]
        check('no renderer or protocol failure markers', not failures, failures)
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
