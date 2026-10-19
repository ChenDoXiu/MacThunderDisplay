#!/usr/bin/env python3
"""Install an unpacked ThunderDisplay bundle for the current user."""
import os
import shutil
from pathlib import Path

source = Path(__file__).resolve().parent
data = Path(os.environ.get('XDG_DATA_HOME', str(Path.home()/'.local/share'))).expanduser().resolve()
target = data/'thunderdisplay'
if source != target:
    shutil.copytree(source, target, dirs_exist_ok=True)
applications = data/'applications'
applications.mkdir(parents=True, exist_ok=True)
def quoted(path):
    value = str(path).replace('\\', '\\\\').replace('"', '\\"').replace('`', '\\`').replace('$', '\\$')
    return '"'+value+'"'
(applications/'thunderdisplay.desktop').write_text(f'''[Desktop Entry]
Type=Application
Name=ThunderDisplay
Comment=macOS LAN extended display
Exec={quoted(target/'ThunderDisplay')}
Icon={target/'share/icons/hicolor/scalable/apps/thunderdisplay.svg'}
Terminal=false
Categories=Utility;RemoteAccess;
StartupWMClass=ThunderDisplayClient
''')
print('Installed:', target)
