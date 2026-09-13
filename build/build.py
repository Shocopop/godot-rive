"""Build the pinned Rive GPU runtime and Windows Godot extension (no Skia)."""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=['debug', 'release'], default='debug')
    parser.add_argument('--skip-rive', action='store_true')
    args, scons_args = parser.parse_known_args()
    root = Path(__file__).resolve().parent.parent
    if sys.platform != 'win32':
        parser.error('The GPU backend currently supports Windows/Vulkan only.')
    env = os.environ.copy()
    env['RIVE_PREMAKE_ARGS'] = '--with_rive_text --with_rive_layout'
    env['GIT_CONFIG_COUNT'] = '1'
    env['GIT_CONFIG_KEY_0'] = 'core.longpaths'
    env['GIT_CONFIG_VALUE_0'] = 'true'
    if not args.skip_rive:
        subprocess.run(['sh', '../build/build_rive.sh', 'release', '--with_vulkan',
            '--with-rtti', '--with-exceptions', '--no-lto', '--no_gl', '--',
            'rive', 'rive_pls_renderer', 'rive_decoders', 'rive_harfbuzz',
            'rive_sheenbidi', 'rive_yoga', 'libwebp', 'libpng', 'zlib', 'libjpeg'],
            cwd=root / 'thirdparty/rive-cpp/renderer', env=env, check=True)
    subprocess.run([sys.executable, '-m', 'SCons', 'platform=windows', 'arch=x86_64',
        f'target=template_{args.target}', 'use_static_cpp=yes'] + scons_args,
        cwd=root / 'build', env=env, check=True)
    print('Build successful.')


if __name__ == '__main__':
    main()
