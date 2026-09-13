"""Release entry point for the automatic recovery Lua regression."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).with_name('test_auto_sync_lua.py')), run_name='__main__')
