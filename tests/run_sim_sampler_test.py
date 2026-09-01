"""Build and test the real simulation sampler in a hidden GLFW window (Windows/MSVC).

All build products, runtime state and logs go under the system temporary directory.
No hardware connection is made. Optional argument: an alternative build directory.
If the neighbouring simulation executable exists, also verifies the complete
reference-command loop against a separate real MuJoCo process.
"""
import os
import argparse
from pathlib import Path
import subprocess
import tempfile
import re

project = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("build_directory", nargs="?", type=Path,
                    default=Path(tempfile.gettempdir()) / "ava-sim-tool-build")
simulation = project.parent / "motion_control/mujoco_single_joint"
parser.add_argument("--simulation-exe", type=Path, default=simulation / "build/Release/single_joint.exe")
parser.add_argument("--model", type=Path, default=simulation / "model.xml")
parser.add_argument("--dock-session", action="store_true", help="Run dock selection/session restoration regression instead")
parser.add_argument("--wave-input", action="store_true", help="Run Enter-only wave input regression")
parser.add_argument("--local-file-server", action="store_true", help="Run local HTTP file server and updater integration regression")
parser.add_argument("--variable-properties", action="store_true", help="Run unified variable/member properties popup regression")
parser.add_argument("--variable-focus", action="store_true", help="Run variable manager/monitor double-click focus, editing, navigation and sampling regression")
parser.add_argument("--system-tray", action="store_true", help="Run Windows system tray and background sharing regression")
parser.add_argument("--fft", action="store_true", help="Run FFT startup/session restoration regression")
parser.add_argument("--mujoco", action="store_true", help="Run embedded MuJoCo rendering and command/telemetry regression")
parser.add_argument("--update-versions", action="store_true", help="Run updater version dropdown and selected download regression")
parser.add_argument("--inverter", action="store_true", help="Run inverter and device switching regression")
parser.add_argument("--plot-axes", action="store_true", help="Run independent Y-axis input and fit padding regression")
parser.add_argument("--discard-range", action="store_true", help="Run Scope time-interval selection, discard and paused sampler regression")
parser.add_argument("--new-session", action="store_true", help="Run concurrent new-session teardown regression")
parser.add_argument("--session-reopen", action="store_true", help="Run startup session chooser and session persistence regression")
parser.add_argument("--rtt-reconnect", action="store_true", help="Run RTT reconnection and connection tooltip checks with an in-process SDK fixture")
parser.add_argument("--csv-export", action="store_true", help="Run scoped CSV file export and range menu regression")
parser.add_argument("--dso", action="store_true", help="Run DSOX2024A GUI against a loopback SCPI fixture")
parser.add_argument("--three-phase", action="store_true", help="Run real SPICE three-phase inverter and interactive probe regression")
parser.add_argument("--flash-read", action="store_true", help="Run Flash hex viewer and automatic remote address regression")
parser.add_argument("--input-select-all", action="store_true", help="Run input cursor placement and explicit select-all regression")
parser.add_argument("--bode-ui", action="store_true", help="Run Bode cursor, fit and automatic analysis regression")
args = parser.parse_args()
test_name = "input_select_all_test" if args.input_select_all else "flash_read_test" if args.flash_read else "three_phase_test" if args.three_phase else "plot_axes_test" if args.plot_axes else "inverter_sim_test" if args.inverter else "mujoco_view_test" if args.mujoco else "updater_versions_test" if args.update_versions else "fft_startup_test" if args.fft else "system_tray_test" if args.system_tray else "variable_properties_test" if args.variable_properties else "local_file_server_test" if args.local_file_server else "wave_input_test" if args.wave_input else "dock_session_test" if args.dock_session else "sim_sampler_test"
output = args.build_directory
if args.variable_focus:
    test_name = "variable_focus_test"
if args.discard_range:
    test_name = "time_range_discard_ui_test"
if args.new_session:
    test_name = "new_session_test"
if args.session_reopen:
    test_name = "session_reopen_test"
if args.rtt_reconnect:
    test_name = "rtt_reconnect_test"
if args.csv_export:
    test_name = "csv_export_scope_test"
if args.dso:
    test_name = "dso_scope_test"
if args.bode_ui:
    test_name = "bode_ui_test"
output = output.resolve()
binary = output.parent / "ava-sim-tool-bin"
output.mkdir(parents=True, exist_ok=True)
options = ["BUILD_DIR=" + output.as_posix(), "BIN_DIR=" + binary.as_posix()]
log = output / "sim-sampler-build.log"
with log.open("w", encoding="utf-8") as stream:
    build = subprocess.run(["make", "-j4", *options], cwd=project, stdout=stream, stderr=subprocess.STDOUT)
if build.returncode:
    print(log.read_text(encoding="utf-8", errors="replace"))
    raise SystemExit(build.returncode)

# Obtain the project's exact compiler and linker settings; replace only its entry point.
dry_run = subprocess.check_output(["make", "-nB", *options, "VCVARS_DONE=1"], cwd=project,
                                  text=True, errors="replace")
compile_line = next(line for line in dry_run.splitlines()
                    if line.startswith("cl.exe ") and '"src/main.cpp"' in line)
obj = output / (test_name + ".o")
compile_line = compile_line.removeprefix("cl.exe ").replace(
    '/Fo"' + (output / "msvc-win-x86_64/src/main.o").as_posix() + '"', '/Fo"' + obj.as_posix() + '"')
compile_line = compile_line.replace('"src/main.cpp"', '"tests/' + test_name + '.cpp"')
link_line = next(line for line in dry_run.splitlines()
                 if line.startswith("link.exe ") and "/win/ava_tool.exe" in line)
link_line = link_line.removeprefix("link.exe ").replace("/SUBSYSTEM:WINDOWS", "/SUBSYSTEM:CONSOLE")
link_line = link_line.replace("/win/ava_tool.exe", "/win/" + test_name + ".exe")
link_line = link_line.replace((output / "msvc-win-x86_64/src/main.o").as_posix(), obj.as_posix())
if args.rtt_reconnect:
    # This test includes the implementation to replace its private DLL dispatch.
    implementation = (output / "msvc-win-x86_64/src/core/jlink_port.o").as_posix()
    link_line = link_line.replace('"' + implementation + '"', '').replace(implementation, '')
# Tests do not use the application's elevated hardware entry point.
link_line = re.sub(r'/MANIFESTINPUT:"[^"]*"', '', link_line)
link_line = re.sub(r'/MANIFESTUAC:"[^"]*"', '/MANIFESTUAC:"level=\'asInvoker\' uiAccess=\'false\'"', link_line)
compile_rsp, link_rsp = output / "sim-compile.rsp", output / "sim-link.rsp"
compile_rsp.write_text(compile_line, encoding="utf-8")
link_rsp.write_text(link_line, encoding="utf-8")
vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
vs = subprocess.check_output([str(vswhere), "-latest", "-products", "*", "-requires",
                              "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property",
                              "installationPath"], text=True).strip()
batch = output / "sim-test-build.cmd"
batch.write_text('@echo off\ncall "' + str(Path(vs) / "VC/Auxiliary/Build/vcvarsall.bat") + '" x64 >nul\n'
                 'if errorlevel 1 exit /b 1\ncl.exe @"' + str(compile_rsp) + '"\n'
                 'if errorlevel 1 exit /b 1\nlink.exe @"' + str(link_rsp) + '"\n'
                 'exit /b %errorlevel%\n', encoding="utf-8")
subprocess.run(["cmd.exe", "/d", "/c", os.path.relpath(batch, project)], cwd=project, check=True)
exe = binary / ("win/" + test_name + ".exe")
test_args = [str(exe), str(output / (test_name + ".ava"))]
if args.csv_export:
    pass
elif args.dso:
    pass
elif args.mujoco:
    test_args += [str(project / "assets/mujoco_single_joint.xml"), str(output / "mujoco.ppm")]
elif not args.new_session and not args.bode_ui and not args.rtt_reconnect and not args.session_reopen and not args.discard_range and not args.variable_focus and not args.input_select_all and not args.flash_read and not args.three_phase and not args.plot_axes and not args.inverter and not args.update_versions and not args.fft and not args.system_tray and not args.variable_properties and not args.local_file_server and not args.dock_session and not args.wave_input and args.simulation_exe.is_file() and args.model.is_file():
    test_args += [str(args.simulation_exe.resolve()), str(args.model.resolve())]
if args.dso:
    import dso_fixture
    server = dso_fixture.start()
    try:
        test_args.append(str(server.server_address[1]))
        subprocess.run(test_args, cwd=exe.parent, timeout=60, check=True)
    finally:
        server.shutdown()
        server.server_close()
elif args.local_file_server:
    import auth_update_fixture
    auth_server = auth_update_fixture.start()
    try:
        test_args.append("http://127.0.0.1:" + str(auth_server.server_address[1]))
        subprocess.run(test_args, cwd=exe.parent, timeout=150 if args.three_phase else 90 if args.inverter else 45, check=True)
    finally:
        auth_server.shutdown()
        auth_server.server_close()
else:
    subprocess.run(test_args, cwd=exe.parent, timeout=240 if args.three_phase else 90 if args.inverter else 45, check=True)
