#!/usr/bin/env python3
"""Build a relocatable Linux client bundle from the local CMake build."""
import argparse
import os
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

def run(args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build-dir', type=Path, default=ROOT/'build/linux')
    parser.add_argument('--output', type=Path, default=ROOT/'build/dist')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    architecture = platform.machine()
    name = f'ThunderDisplay-linux-{architecture}'
    with tempfile.TemporaryDirectory(prefix='thunderdisplay-') as temporary:
        bundle = Path(temporary)/name
        subprocess.run(['cmake', '--install', str(args.build_dir.resolve()), '--prefix', str(bundle)], check=True)
        for directory in ('plugins', 'licenses', 'share/applications', 'share/icons/hicolor/scalable/apps'):
            (bundle/directory).mkdir(parents=True, exist_ok=True)
        plugin_root = None
        for command in ('qtpaths6', 'qtpaths', '/usr/lib/qt6/bin/qtpaths'):
            try:
                candidate = Path(run([command, '--plugin-dir']).strip())
                if candidate.is_dir():
                    plugin_root = candidate
                    break
            except (OSError, subprocess.CalledProcessError):
                pass
        if plugin_root is None:
            raise SystemExit('Qt plugin directory not found; install qtpaths from Qt 6.')
        plugins = []
        for directory in ('platforms', 'wayland-decoration-client', 'wayland-graphics-integration-client', 'wayland-shell-integration', 'xcbglintegrations', 'iconengines'):
            source = plugin_root/directory
            if not source.is_dir():
                continue
            target = bundle/'plugins'/directory
            target.mkdir(parents=True)
            for file in source.glob('*.so'):
                if directory == 'platforms' and file.name not in ('libqwayland.so', 'libqxcb.so', 'libqoffscreen.so'):
                    continue
                if directory == 'iconengines' and file.name != 'libqsvgicon.so':
                    continue
                shutil.copy2(file, target/file.name)
                plugins.append(target/file.name)
        # Leave libc, the Vulkan loader and display/GPU drivers to the host.
        system = re.compile(r'^(ld-linux|lib(c|m|pthread|dl|rt|resolv|anl)\.so|libnss_|lib(vulkan|GL|EGL|OpenGL|GLES|glapi|gbm|drm))')
        queue = list((bundle/'bin').iterdir()) + plugins + list((bundle/'lib').iterdir())
        checked = set()
        original_files = set()
        while queue:
            file = queue.pop()
            if file in checked:
                continue
            checked.add(file)
            listing = run(['ldd', str(file)])
            if 'not found' in listing:
                raise SystemExit(f'Missing dependencies for {file.name}:\n{listing}')
            for line in listing.splitlines():
                match = re.match(r'\s*(\S+) => (/\S+)', line)
                if not match or system.match(match[1]):
                    continue
                soname, source = match[1], Path(match[2])
                original_files.add(source.resolve())
                target = bundle/'lib'/soname
                if not target.exists():
                    shutil.copy2(source, target)
                    queue.append(target)
        for plugin in plugins:
            original_files.add((plugin_root/plugin.parent.name/plugin.name).resolve())
        packages = set()
        for file in original_files:
            try:
                if shutil.which('pacman'):
                    packages.add(run(['pacman', '-Qoq', str(file)]).strip())
                elif shutil.which('dpkg-query'):
                    package = run(['dpkg-query', '-S', str(file)]).split(': /', 1)[0]
                    packages.add(package)
            except subprocess.CalledProcessError:
                pass
        for package in packages:
            source = Path('/usr/share/licenses')/package
            if source.is_dir():
                shutil.copytree(source, bundle/'licenses'/package, dirs_exist_ok=True)
            else:
                source = Path('/usr/share/doc')/package/'copyright'
                if source.is_file():
                    target = bundle/'licenses'/package
                    target.mkdir(exist_ok=True)
                    shutil.copy2(source, target/'copyright')
        license_file = ROOT/'vendor/pyrowave/LICENSE'
        if license_file.is_file():
            shutil.copy2(license_file, bundle/'licenses/PyroWave-LICENSE')
        shutil.copy2(ROOT/'vendor/stb/LICENSE', bundle/'licenses/stb-LICENSE')
        shutil.copy2(ROOT/'assets/thunderdisplay.svg', bundle/'share/icons/hicolor/scalable/apps/thunderdisplay.svg')
        (bundle/'bin/qt.conf').write_text('[Paths]\nPrefix=..\nPlugins=plugins\nLibraries=lib\n')
        launcher = bundle/'ThunderDisplay'
        launcher.write_text('''#!/bin/sh
set -eu
TD_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$TD_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$TD_DIR/plugins"
if [ -z "${QT_QPA_PLATFORM:-}" ]; then
  if [ -n "${WAYLAND_DISPLAY:-}" ]; then export QT_QPA_PLATFORM="wayland;xcb"; else export QT_QPA_PLATFORM=xcb; fi
fi
exec "$TD_DIR/bin/thunder-display-client" "$@"
''')
        launcher.chmod(0o755)
        shutil.copy2(ROOT/'scripts/install-linux.py', bundle/'install.py')
        install = bundle/'install.sh'
        install.write_text('exec python3 "$(dirname -- "$0")/install.py" "$@"\n')
        install.write_text('#!/bin/sh\n'+install.read_text())
        install.chmod(0o755)
        libc = run(['getconf', 'GNU_LIBC_VERSION']).strip()
        (bundle/'BUILD-INFO.txt').write_text(f'ThunderDisplay 0.2.0\nArchitecture: {architecture}\nBuild runtime: {libc}\nQt, SDL3, PyroWave, FFmpeg and dependency libraries are bundled.\nHost Vulkan loader and GPU drivers are required.\nThis binary requires a compatible Linux runtime; rebuild on older distributions.\n')
        shutil.copy2(ROOT/'LICENSE', bundle/'licenses/ThunderDisplay-LICENSE')
        shutil.copy2(ROOT/'THIRD_PARTY_NOTICES', bundle/'licenses/THIRD_PARTY_NOTICES')
        archive = args.output/f'{name}.tar.gz'
        with tarfile.open(archive, 'w:gz', compresslevel=6) as output:
            output.add(bundle, arcname=name)
        print(archive)

if __name__ == '__main__':
    main()
