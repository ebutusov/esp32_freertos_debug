# gdb/vscode_setup.gdb - sourced by .vscode/launch.json BEFORE connecting.
#
# VS Code starts GDB directly, without the ESP-IDF environment activated.
# GDB's embedded Python then cannot find the freertos-gdb package (it lives
# in the ESP-IDF Python venv), so we add the venv to sys.path ourselves.
python
import os, sys
venv = os.environ.get("IDF_PYTHON_ENV_PATH") or "C:/Espressif/tools/python/v6.1/venv"
site = os.path.join(venv, "Lib", "site-packages")
if site not in sys.path:
    sys.path.append(site)
end

# Same files `idf.py gdb` uses, minus "connect" (VS Code does target remote).
source build/gdbinit/py_extensions
source build/gdbinit/symbols

set print pretty on
