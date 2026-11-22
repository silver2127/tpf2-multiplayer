"""Compile and run net_rto_resilience_test.cpp using msvc_env.bat.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / '.local-test/tests/net-rto-resilience'
out.mkdir(parents=True, exist_ok=True)

vcvars = root / 'tools' / 'msvc_env.bat'
(out / 'build.cmd').write_text(f'@echo off\ncall "{vcvars}" || exit /b 1\n'
    f'cl /nologo /EHsc /W4 "{root / "tools" / "net_rto_resilience_test.cpp"}" /Fe:test.exe >build.log 2>&1\n'
    'if errorlevel 1 (type build.log & exit /b 1)\n'
    'exit /b 0\n', encoding='utf-8')

print("Building net_rto_resilience_test...")
subprocess.run(['cmd', '/d', '/c', str(out / 'build.cmd')], cwd=out, check=True, timeout=180)
print("Running net_rto_resilience_test...")
subprocess.run([str(out / 'test.exe')], cwd=out, check=True, timeout=30)
