# MOSFET / GaN SPICE tool

Open **Tools → Inverter Simulation / 工具 → 逆变器仿真**, then run ngspice.
Choose **Circuit → Three-phase inverter + windings** for the interactive inverter,
or **Double-pulse test** for device characterization. New docks default to the
three-phase circuit; old sessions keep their saved double-pulse setup.
The tool generates the selected testbench and delegates the operating point
and transient solve to the **ngspice 47** console executable. It contains no
custom transistor solver and no IGBT mode. The child process runs hidden, off
the GUI thread, with cancel support and a 60-second limit. Session files retain
model/parameter selections and dock visibility without automatically running.

The three-phase circuit contains three half bridges (six MOSFETs, or three
ISG0616N10NM5HSC integrated packages), external bus/source/gate parasitics,
complementary center-aligned sampled sinusoidal PWM, finite driver edges and
turn-on dead time. The fixed-speed motor load consists of three identical,
uncoupled R-L windings connected to a floating star point; its 1 Gohm reference
resistor prevents a singular floating node. Sinusoidal phase EMF is imposed by
SPICE behavioral voltage sources, with a 1 us startup ramp. Electrical frequency
is `rpm * polePairs / 60`. EMF peak is specified independently at the configured
speed; zero speed makes EMF zero and negative speed reverses EMF polarity and
rotation. Voltage lead adjusts PWM phase relative to EMF. This is an open-loop,
fixed-speed electrical load, without a mechanical solve, mutual inductance,
saliency, magnetic saturation or FOC controller.

The bridge-to-winding wires use clickable horizontal/vertical line segments.
The three-phase winding is drawn as a circular motor to the right of the bridge,
with three phase coils and a central star point N. This drawing represents the
same lumped R-L-EMF model; the visual coil turns do not change inductance or slot
geometry. Clicking a coil offers winding voltage/current and its R/L/EMF probes.

The **Probe preset** menu replaces the current selection with common combinations:
QAL (A low-side VDS/VGS/ID), QAH (A high-side VDS/VGS/ID), three low-side VDS/VGS,
three phase currents, winding phase voltages/currents, or DC bus/line voltages.
Individual probes can still be added or removed by clicking the diagram. Selected
MOS devices are highlighted, and presets/selections persist in the session.

The schematic is a fixed circuit with selectable probes, rather than a general
schematic editor. Clicking dots or supported wire segments toggles voltage to
ground. Clicking a MOS opens VDS/VGS/ID choices; the circular windings offer R, L
and EMF voltages and phase current. DC current, neutral potential and line-to-line
voltages are also available. Current sign is explicitly shown: MOS drain to
source, phase bridge to winding, bus supply to bridge. Voltage and current probes
use separate shared monitor scopes. All available probes are captured during the
run, so changing probe selection does not rerun SPICE. CSV contains all probes,
not only those selected for display. Saved sessions retain the probe selections.
Comparison overlays the same probes from the previous successful three-phase
run; different circuit topologies are not overlaid. Metrics show peak VDS,
peak phase current and signed DC input energy. DC energy includes winding and
field energy exchange and cannot be interpreted as transistor heat loss.

The three-phase timing inputs are PWM frequency and an integer carrier cycle
count. Defaults are 20 kHz and 2 cycles: 100 us of PWM plus a 1 us initialization
interval, for a total of 101 us. The UI displays the derived period and total
duration; there is no independent duration input. Old duration-only sessions
migrate by rounding up to sufficient complete carrier cycles.
With 50 ns maximum solver steps, increase cycle count to inspect slower electrical
cycles and check convergence by reducing the maximum step. Runs are limited to
500000 adaptive output rows, 500000 requested maximum steps, 10000 PWM periods,
and 60 seconds. The minimum duty interval must accommodate dead time and edge
time; the maximum step cannot exceed 2% of a carrier period.

`python scripts/setup_ngspice.py` installs the official Windows runtime under
`thirdparty/ngspice/Spice64`, verifying the official archive SHA-256. 7-Zip is
required. The build copies the runtime and its license documents beside the app;
the installer includes them. An existing executable can instead be selected in
the dock. Use `ngspice_con.exe` for the console build.

The default preset uses the user-supplied Infineon library:

```
OptiMOS5/OptiMOS5_100V_Halfbridges_LTSpice.lib
.SUBCKT ISG0616N10NM5HSC_L1 VIN VSW GND GATEH GATEL
```

This is the electrical half-bridge model for ISG0616N10NM5HSC; ATMA1 is the order
suffix. Select the integrated-package topology and map pins, in declaration
order, to **BUS SW S HG G**. Model provenance, version and file hash are retained
in `assets/spice/infineon_optimos_100v/README.md`. The original vendor comments
remain unchanged. The alternative same-name model exposes thermal nodes and
requires a suitable thermal circuit; the preset uses `_L1`.

MOSFET and GaN use the selected **manufacturer subcircuit**. The model's physics
comes from the file, not the device dropdown. GaN has no synthetic fallback.
The explicitly selected generic MOS demonstration uses ngspice's built-in VDMOS
model and is labeled as a demonstration, never as a named part. Encrypted or
incompatible libraries fail with the ngspice log; importing a file does not
guarantee engine compatibility. Library `.SUBCKT` declarations populate the
model selector. Map pins explicitly for other parts. Standard single-device
roles are D/G/S/K; integrated half-bridge roles are BUS/HG/SW/G/S/K. TJ/TC roles
connect to the configured fixed temperature for compatible electrothermal
models; arbitrary multi-node heat-sink networks are not constructed automatically.

The low-side device receives two finite-edge gate pulses, while the high-side
device is held off. Gate resistance, gate-loop inductance, common-source
inductance, commutation-loop inductance/resistance and load inductance are
external circuit elements. Set drive voltage according to the actual device.
The SPICE operating point preloads the requested inductor current using an
initialization voltage source; its coefficient decays with a 1 ns time constant
and is negligible before the first gate pulse at 0.5 us. This avoids setting
uninitialized internal device capacitances with `uic`.

The dock puts waveforms and comparison on the left, model and circuit parameters
and run controls on the right. Drag the column divider to resize the panels;
each panel scrolls independently. Comparison details are collapsed initially.

Plots use the variable monitor's existing MonitorScope/MonitorChannel components,
including curve settings, Y fitting, linked X navigation, table view and stats.
SPICE samples are loaded into recorded channels with microsecond timestamps and
no history pruning. Original solver doubles are retained for CSV and energy
comparison; display channels use the monitor's float value storage. Live sampling
and FFT controls are unavailable for these adaptive-step recorded waveforms.

Plots share a movable time axis and show VGS, VDS, device/load/freewheel currents,
signed terminal power and cumulative signed terminal energy. The plotted
extrema are preserved during display downsampling. CSV export retains all
adaptive solver samples. Voltage is measured across the actual package pins.
In the integrated package, low-side drain current is inferred from source
current minus gate current. For separate devices, a zero-volt drain probe
measures current directly.

After two successful runs, the dock overlays the current result with the previous
successful result. Previous curves use thinner, faded lines; the overlay can be
disabled. Both use absolute simulation time, so changing pulse durations shifts
the corresponding edges. Double-clicking the waveform area fits the complete
recorded time range, including both runs when the overlay is enabled; there is
no separate View all button. CSV export uses the shared green action style.
The comparison table shows VDS peak and signed Eon/Eoff, absolute changes,
percentage changes relative to the magnitude of the previous value, and changed
numeric parameters. A zero baseline has no percentage. Model changes are also
shown. Parameter snapshots belong to completed results, not the editable inputs.
Starting, cancelling or failing a run preserves the displayed successful results.
Comparison history is held only for this application session; CSV exports the
current successful run.

Energy summaries integrate `VDS * ID` in a **fixed 0.5 us window** following the
second rising driver edge and first falling driver edge. These contain reactive
capacitance energy exchange and any conduction in the window. They are not
manufacturer-standard switching-energy extractions or a thermal solution.
The exact circuit, full solver log and result file are preserved in the displayed
temporary job directory. Maximum timestep and convergence tolerances apply to
the SPICE solver; parameter validity and complete finite output are checked.

Run the real engine/model/GUI regression with:

```powershell
python tests/run_sim_sampler_test.py --inverter
```

Sources: [ngspice downloads](https://ngspice.sourceforge.io/download.html),
[Infineon device page](https://www.infineon.com/part/ISG0616N10NM5HSC),
[EPC manufacturer GaN models](https://epc-co.com/epc/design-support/device-models/models).
