# MuJoCo simulation dock

Choose **Tools -> MuJoCo** to open one dock containing the interactive 3D scene,
position, velocity, torque and tracking-error plots, reference targets and PD
parameters. Waveforms appear on the left; the interactive model and target/PD
controls appear on the right. Drag the vertical column divider to adjust their
widths. Waveforms and controls scroll independently. The bundled single-joint
XML loads as a preview. Click **Start
simulation / 启动仿真** to run it inside AvA Tool; no separate simulation window
is needed. Left drag rotates, right drag pans and the wheel zooms. Hold Shift
to switch the rotation/pan axis; middle drag also zooms. The image captures mouse
drags so they do not move the dock or scroll the plots. Its offscreen framebuffer
uses the actual display/DPI pixel size, with multisample antialiasing, instead of
stretching a capped 1280x720 image. The draggable
divider changes the scene height. Pause, reset and disturbance torque controls
are in the scene toolbar. Reset starts a new telemetry epoch, clears targets and
retains the control mode and gains. Closing the dock stops its built-in simulation.

The right-hand control panel offers three modes:

- **Position**: position Kp, target position, velocity feedforward and torque
  feedforward. The position P loop produces a velocity demand, adds velocity
  feedforward, then uses the velocity PI loop to produce torque.
- **Velocity**: velocity Kp/Ki, target velocity and torque feedforward. These
  PI gains are also used by the inner velocity loop in Position mode.
- **PD**: Kp/Kd, target position, target velocity and torque feedforward. Torque
  is `Kp * position error + Kd * velocity error + feedforward`.

References are output-joint radians, rad/s and Nm. All modes add torque
feedforward before actuator torque limiting. PI uses conditional integration
against those actual limits; changing mode or Ki clears its integral state.
Starting simulation preserves entered targets and feedforward; explicit reset
zeros them and clears integration. Sessions preserve all mode gains, targets,
feedforward and the selected mode without auto-starting physics. Parameters have
hover explanations. The advanced SHM command tables remain available under a
collapsed section. Existing q_ref/dq_ref/kp/kd commands remain supported, with
Kp applied to the active mode and dq_ref used as velocity feedforward in Position
mode. Velocity-mode error telemetry is velocity error; other modes use position
error. The marker uses the existing reserved packet field and preserves packet
size/compatibility with external producers.

The chosen position cascade follows the standard position/velocity arrangement
described in [Synapticon's position control documentation](https://doc.synapticon.com/node/sw5.0/motion_control/control_loops/position_loop.html?TocPath=Software+Reference+5.0%7CMotion+Control%7CControl+Loops%7C_____3).

While built-in simulation is running, **Ctrl + left drag** on a moving body
applies a force and **Ctrl + right drag** applies a rotational perturbation.
Shift changes the drag plane/axis. Releasing the mouse/Ctrl, losing focus,
hiding the scene, resetting or stopping removes the mouse force. Clicking fixed
scenery or missing a body does not accidentally rotate the camera. External
simulation mode supports camera controls; physical perturbation requires the
built-in simulation.

The 3D scene and plots use the same model state and existing SHM command/telemetry
protocol. Editing q_ref/dq_ref/kp/kd or using the wave generator controls the
built-in model. Model path and scene height are saved in `.ava`; reopening a
session shows the preview and does not automatically start physics.

The renderer currently supports Windows single-joint, single-actuator models.
Build with `MUJOCO_DIR` pointing to a MuJoCo SDK (default `C:/mujoco`). The build
copies its matching DLL and notices beside the application. If the SDK is absent,
ordinary waveform monitoring remains available and the scene reports the missing
renderer. DLL/header versions must match.

An existing neighbouring `motion_control/mujoco_single_joint` simulation can
still publish telemetry. When internal physics is stopped, the scene follows
its measured joint position. Load the same XML used by that simulation. A second
producer for the same region is rejected; stop the external simulation before
starting internal physics.

The monitor uses the `SIM_SHM` device, separate from scalar `SHM`. Each channel
maps a frame field (`q_ref`, `q`, `dq_ref`, `dq`, `tau`, `error`, `kp`, `kd`) from the shared
region `Local\AvA.MuJoCo.SingleJoint.v1`. The shared-memory name and field can be
changed through channel properties; `.ava` sessions retain both settings.

## Set reference targets

Use the default **Reference Targets / 目标设置** table above the waveforms, edit
**q_ref** in its Value column and press **Enter** or click out of the input.
Existing Position scopes can also use TABLE. The default unit is rad
(`0.5` is about 28.6 degrees). **dq_ref**
in the Velocity scope is writable in rad/s and normally stays at zero for a
position target. It is the PD velocity reference, not a separate velocity mode.
The input preserves the text being edited while live samples arrive. If gain
or bias is configured, input uses display units and is converted to raw units.

Edit **kp** and **kd** in the independent **PD Parameters / PD 参数** scope to tune PD in real time. The defaults are
100 and 10; zero is allowed and values are not clamped. Applied gains are echoed
by the simulation. Backspace resets the model and targets but retains the gains.
Older simulation sessions automatically gain missing PD parameter rows; rows
from the combined Targets/PD table move to the independent PD scope.
Restart both applications after upgrading. Telemetry v2 uses 72-byte frames;
the updated AvA reader also accepts the original 64-byte v1 telemetry.

Commands travel through `<SHM name>.commands.v1`, independently of telemetry.
The simulation echoes the applied target through the reference curves and table
values. Targets, joint rotation and actuator torque are not clamped. Commands entered
before the initial connection wait for the first telemetry epoch. Backspace
resets targets to zero; commands from an old epoch are rejected. A `.ava` session
preserves R/W settings without automatically issuing targets when loaded. Older
simulation sessions migrate reference channels to writable.

## Wave generator inputs

The Wave column's **...** button opens the generator for writable simulation
channels (`q_ref`, `dq_ref`, `kp`, `kd`). Turn it ON explicitly to send basic or
dynamic waveforms through the command queue. The generator uses simulation time;
when simulation time stops advancing, no new waveform values are generated.
Applied values continue to come from actual simulation telemetry.

Manual simulation inputs commit on Enter or when leaving the field. A committed
target remains visible until a published simulation snapshot acknowledges it,
including while physics or display publishing is paused. Hovering the input
shows the currently applied value and whether acknowledgment is pending.
A manual write switches off waveform generators for that same region/field,
including generators attached to another scope, so they cannot overwrite it.

All generator numeric inputs, including frequency, amplitude, offset, duty and
dynamic start/end/duration/increment/interval, apply only after **Enter**.
Typing or leaving the field does not change the active configuration. Mode,
wave type, time-law dropdowns and ON/OFF controls apply on selection.

The sampler opens one reader per region and distributes each frame to every
receiving scope. The X axis is simulation time in seconds. A changed simulation
epoch clears old channel history. The simulation's Backspace key starts a new
epoch. Multiple AvA instances cannot consume the same region concurrently.

The toolbar shows whether frames arrived within the past second, received frame
count and cumulative producer drops. Waiting can mean the producer has stopped,
the name is incorrect, another reader owns the region, or the layout is
incompatible. Pausing acquisition drains and discards new frames; pausing the
display retains acquisition. A full queue rejects frames without blocking the
simulation. Drops become visible when a subsequent frame is received.

The protocol and endpoint wrappers are in `ckit/include/sim_stream.hpp`, reusing
`shm.c` and `spsc.c`. The mapping is 1 MiB, with 7281 72-byte telemetry frames in the
512 KiB FIFO. Use matching 64-bit module headers/library on both sides. The
current integration is verified on Windows.

Do not add ordinary SHM variables using either region; they would consume frame
bytes as scalars. q_ref, dq_ref, kp and kd are writable; measured fields are always
read-only. The command protocol supports manual targets and generated waveforms.
Control mode is not exposed.

Build/run commands and automated protocol/physics tests are documented in
`motion_control/mujoco_single_joint/README.md`.

For the actual AvA sampler and session round-trip regression, run:

```powershell
python tests/run_sim_sampler_test.py
```

The test creates a hidden GLFW window and verifies fanout to 32 receiving
channels, simulation timestamps, field selection, position/velocity command
writes, independent monitor pause, epoch reset, legacy migration and `.ava`
save/reload. Build products and runtime settings/logs go
under the system temporary directory; no probe or hardware connection is made.
When the neighbouring `build/Release/single_joint.exe` exists, the test also starts
an isolated headless simulation and verifies the complete command/feedback loop,
including pre-connection writes, real joint motion, unclamped targets and reset.

For the native numeric input regression (typing, focus loss, Enter and invalid input):

```powershell
python tests/run_sim_sampler_test.py --wave-input
```

For the embedded 3D dock, real model rendering, PD commands, pause/reset,
producer ownership and saved model/split-height regression:

```powershell
python tests/run_sim_sampler_test.py --mujoco
```

## Configurable joint actuator

In the MuJoCo dock, choose **New actuator... / 新建关节模组...**, edit parameters,
then **Generate, save and load / 生成、保存并加载**. Start simulation afterward.
The original pendulum remains available. The bundled `mujoco_actuator.xml` is a
7-pole-pair, 12-slot, 9:1 example with an output magnetic encoder.

The generator supports 1–32 pole pairs (exactly 2p magnet segments), 3–96 stator
slots, requested reduction 2.5–100, and 8–24 encoder bits. Fixed-ring planetary
stages use three equally spaced planets, `Nr = Ns + 2 Np`, and
`(Nr + Ns) % 3 == 0`. Each stage has reduction `1 + Nr/Ns`; stages multiply.
The builder displays actual integer tooth counts and realized reduction;
combinations must be within 0.5% of the request. Higher reductions use up to
three stages. Rectangular tooth geometry illustrates engagement; it is not an
involute manufacturing profile. The exploded view separates stage layers to
expose the assembly; it does not change reduction or reflected inertia.

Output angle is the single dynamic coordinate. Rotor and gear bodies are
kinematic visualizations driven by that coordinate, with correct fixed-ring
planetary speed relations. Rotor inertia is reflected onto the output as
`Jr * N^2`. The actuator transmission is `N * efficiency`, and motor torque is
limited before applying output torque. PD gains operate in output-axis units.
`tau` reports the limited output actuator torque. Load inertia is configurable.
This is an ideal rigid reducer without backlash, compliance or tooth-contact
forces. PMSM poles/slots generate geometry; there is no winding current,
magnetic-field or FOC electrical simulation.

The output encoder supports nominal axial air gap, axial assembly offset,
radial eccentricity, sensor-board tilt and zero offset. Millimetres and degrees
are converted explicitly. Effective center gap is nominal gap plus axial
offset and must exceed 0.02 mm. Radial disk offset and board tilt appear in the
3D model. Quantized measured angle is used for `q` and the PD position loop;
`dq` is the derivative of the continuous error model before quantization.
The smooth approximation is:

```
A = eccentric_mm / 18 * (1 + effective_gap_mm / 2)
q_measured = quantize(q_true + zero_rad + A*sin(q_true)
                     + 0.25*tilt_rad^2*sin(2*q_true))
```

This is a deterministic illustrative magnetic encoder model. Its gap sensitivity
and harmonic coefficients are assumptions, not a calibrated sensor specification.
There is no random noise or magnetic-field solve. Board tilt can reduce local
edge clearance; only center clearance is validated. The default example uses
0.5 mm nominal gap, 0.1 mm axial offset, 0.05 mm eccentricity and 0.2° tilt.
All parameters live in MJCF `<custom><numeric>` values and are restored from XML;
reopening **New actuator...** with an actuator loaded starts from those values.
Generating another model stops the current internal simulation after successful
model compilation; invalid parameters or a failed load retain the current model.

The embedded regression compiles examples at 2.5, 5, 8, 9, 25, 50, 64, 80 and
100 reduction, checks assembly tooth conditions, and verifies real telemetry,
encoder offset, output torque limiting, XML reload and the original pendulum.
