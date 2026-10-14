#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Stage the emulator only, keeping the tested relative asset paths."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SITE = ROOT / 'build/emulator-pages'
WASM = ROOT / 'build/emulator/fm6.wasm'
if not WASM.is_file():
    raise SystemExit('Run bash web/emulator/build.sh first')
if SITE.exists():
    shutil.rmtree(SITE)
(SITE / 'web/emulator').mkdir(parents=True)
(SITE / 'build/emulator').mkdir(parents=True)
for name in ('index.html', 'main.js', 'processor.js'):
    shutil.copy2(ROOT / 'web/emulator' / name, SITE / 'web/emulator' / name)
shutil.copy2(WASM, SITE / 'build/emulator/fm6.wasm')
shutil.copy2(ROOT / 'LICENSE', SITE / 'LICENSE')
shutil.copy2(ROOT / 'LICENSING.md', SITE / 'LICENSING.md')
shutil.copytree(ROOT / 'LICENSES', SITE / 'LICENSES')
subprocess.run(['git', 'archive', '--format=tar.gz', '-o', str(SITE / 'source.tar.gz'), 'HEAD'],
               cwd=ROOT, check=True)
commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
(SITE / 'revision.txt').write_text(commit + '\n')
(SITE / '.nojekyll').touch()
(SITE / 'index.html').write_text('''<!doctype html>
<html lang="es"><meta charset="utf-8"><title>TamaMPE · FM6</title>
<h1>TamaMPE · FM6</h1>
<p>Herramienta auxiliar de pruebas del firmware M-VAVE FM-1.</p>
<p><a href="web/emulator/">Abrir el emulador FM6</a></p>
<p><a href="source.tar.gz">Código fuente correspondiente</a> ·
<a href="LICENSE">Licencia GPL</a> · <a href="LICENSING.md">Licencias y atribuciones</a></p>
</html>
''')
print(f'Pages artifact: {SITE}, revision {commit}')
