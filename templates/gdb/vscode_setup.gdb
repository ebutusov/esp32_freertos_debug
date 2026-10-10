# TEMPLATE - copy to gdb/vscode_setup.gdb and replace the placeholder:
#   <IDF_PYTHON_ENV>  the ESP-IDF Python virtual environment. In an activated
#                     ESP-IDF shell:
#                       PowerShell:  $env:IDF_PYTHON_ENV_PATH
#                       bash/zsh:    echo $IDF_PYTHON_ENV_PATH
#                     Use forward slashes, also on Windows.
#
# Sourced by .vscode/launch.json before connecting. VS Code starts GDB
# without the ESP-IDF environment, so GDB's embedded Python cannot find the
# freertos-gdb package; we add the venv's site-packages to sys.path.
python
import glob, os, sys
venv = os.environ.get("IDF_PYTHON_ENV_PATH") or "<IDF_PYTHON_ENV>"
# Windows venvs use Lib/site-packages, Linux/macOS lib/pythonX.Y/site-packages
for site in glob.glob(os.path.join(venv, "Lib", "site-packages")) + \
            glob.glob(os.path.join(venv, "lib", "python*", "site-packages")):
    if site not in sys.path:
        sys.path.append(site)
end

# Same files `idf.py gdb` uses, minus "connect" (VS Code does target remote).
source build/gdbinit/py_extensions
source build/gdbinit/symbols

set print pretty on
