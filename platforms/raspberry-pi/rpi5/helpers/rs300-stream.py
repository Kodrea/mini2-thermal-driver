#!/usr/bin/env python3
"""mini2 thermal stream — fullscreen Gtk viewer with keyboard controls.

Drives the rs300 V4L2 driver on a Raspberry Pi 5. Resolution is auto-detected
from the sensor subdev at startup (supports 256x192, 384x288, 640x512 YUYV
@ 60fps). gtksink embeds the GStreamer video as a native Gtk widget so the
surrounding window receives key events directly. v4l2 control changes go
through v4l2-ctl subprocess calls on the sensor subdev. Keys are accepted from
either the fullscreen video window OR the launching terminal (when stdin is a
tty).

Logging: every v4l2-ctl call is logged to stderr and appended to
/tmp/rs300-stream.log (file is opened at startup; each run appends).
"""
import atexit
import datetime
import os
import signal
import subprocess
import sys
import termios
import tty


def _setup_display_env():
    """Find the user's display server when launched from a non-interactive
    shell (typical when invoked via ssh). Sets WAYLAND_DISPLAY +
    XDG_RUNTIME_DIR if a Wayland socket exists for this UID, else falls back
    to DISPLAY=:0 + XAUTHORITY for XWayland."""
    if os.environ.get('WAYLAND_DISPLAY') or os.environ.get('DISPLAY'):
        return
    runtime = f'/run/user/{os.getuid()}'
    os.environ.setdefault('XDG_RUNTIME_DIR', runtime)
    try:
        sockets = sorted(e for e in os.listdir(runtime)
                         if e.startswith('wayland-')
                         and e[len('wayland-'):].isdigit())
    except FileNotFoundError:
        sockets = []
    if sockets:
        os.environ['WAYLAND_DISPLAY'] = sockets[0]
        return
    os.environ['DISPLAY'] = ':0'
    xauth = os.path.expanduser('~/.Xauthority')
    if os.path.exists(xauth):
        os.environ.setdefault('XAUTHORITY', xauth)


_setup_display_env()
os.environ.setdefault('NO_AT_BRIDGE', '1')

# Dependency-install cheatsheet shipped next to this script in the repo.
# Note: install.sh copies only this .py to /usr/local/bin, so on a deployed
# system DEPS_FILE will not exist — the failure handler must still work
# without it.
DEPS_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         'rs300-stream-deps.txt')

# The apt command, kept in sync with rs300-stream-deps.txt by hand. Duplicated
# here so the failure message is self-contained when DEPS_FILE is absent.
DEPS_INSTALL_CMD = (
    'sudo apt install -y python3-gi python3-gi-cairo gir1.2-gtk-3.0 '
    'gir1.2-gstreamer-1.0 gir1.2-gst-plugins-base-1.0 '
    'gstreamer1.0-plugins-base gstreamer1.0-plugins-good '
    'gstreamer1.0-plugins-bad gstreamer1.0-gtk3'
)


def _fail_missing_deps(exc):
    """Print an actionable message and exit when the GTK/GStreamer bindings
    cannot be imported (python3-gi missing, or the gir1.2-* typelibs not
    installed). `exc` is the ImportError or ValueError that was raised.
    """
    lines = [
        'ERROR: rs300-stream needs the GTK 3 / GStreamer 1.0 Python bindings,',
        f'       and they could not be loaded ({type(exc).__name__}: {exc}).',
        '',
        'Install them with:',
        '',
        f'  {DEPS_INSTALL_CMD}',
    ]
    if os.path.exists(DEPS_FILE):
        lines.append('')
        lines.append(f'(also listed in {DEPS_FILE})')
    sys.stderr.write('\n'.join(lines) + '\n')
    sys.exit(1)


try:
    import gi  # noqa: E402
    gi.require_version('Gtk', '3.0')
    gi.require_version('Gdk', '3.0')
    gi.require_version('Gst', '1.0')
    from gi.repository import Gdk, GLib, Gst, Gtk  # noqa: E402
except (ImportError, ValueError) as exc:
    _fail_missing_deps(exc)

DEV_VIDEO = '/dev/video0'
SUBDEV = '/dev/v4l-subdev2'
LOG_FILE = '/tmp/rs300-stream.log'
FPS = 60

# Opened at module load; each run appends so previous logs are preserved.
_log_fh = open(LOG_FILE, 'a')


# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------

def _ts():
    return datetime.datetime.now().strftime('%H:%M:%S.') + \
        f'{datetime.datetime.now().microsecond // 1000:03d}'


def _log(msg):
    line = f'[{_ts()}] {msg}\n'
    sys.stderr.write(line)
    sys.stderr.flush()
    _log_fh.write(line)
    _log_fh.flush()


def _run_v4l2(cmd):
    """Run a v4l2-ctl command, log the call and result, return CompletedProcess."""
    _log(f'CMD: {" ".join(cmd)}')
    result = subprocess.run(cmd, capture_output=True, text=True)
    stdout = result.stdout.strip()
    stderr = result.stderr.strip()
    parts = [f'RC={result.returncode}']
    if stdout:
        parts.append(f'stdout="{stdout}"')
    if stderr:
        parts.append(f'stderr="{stderr}"')
    if not stdout and not stderr:
        parts.append('(no output)')
    _log(' '.join(parts))
    return result


# ---------------------------------------------------------------------------
# Resolution auto-detection
# ---------------------------------------------------------------------------

def _detect_resolution():
    """Query subdev format and return (width, height). Falls back to 384x288."""
    result = _run_v4l2(['v4l2-ctl', '--get-subdev-fmt', 'pad=0', '-d', SUBDEV])
    for line in result.stdout.splitlines():
        line = line.strip()
        if line.startswith('Width/Height'):
            try:
                _, vals = line.split(':', 1)
                w_str, h_str = vals.strip().split('/')
                w, h = int(w_str.strip()), int(h_str.strip())
                print(f'Detected resolution: {w}x{h}', flush=True)
                return w, h
            except (ValueError, IndexError):
                pass
    print('WARNING: could not parse subdev resolution; falling back to 384x288',
          file=sys.stderr, flush=True)
    _log('Resolution parse failed — using fallback 384x288')
    return 384, 288


WIDTH, HEIGHT = _detect_resolution()


def _propagate_csi_format(width, height):
    """Force the Pi 5 CFE CSI-2 receiver pads (/dev/v4l-subdev0) to match the
    rs300 source resolution. The rs300 driver does not push format changes
    through the link, so without this the CFE sees the previous default
    (typically 384x288) on its sink pads and rejects stream-on with
    'Wrong width or height ...'. Workaround until the driver propagates."""
    csi_dev = '/dev/v4l-subdev0'
    if not os.path.exists(csi_dev):
        return
    for pad in (0, 1):
        _run_v4l2(['v4l2-ctl', '--set-subdev-fmt',
                   f'pad={pad},width={width},height={height},code=0x2011',
                   '-d', csi_dev])


_propagate_csi_format(WIDTH, HEIGHT)

PIPELINE = (
    f'v4l2src name=src device={DEV_VIDEO} '
    f'! video/x-raw,format=YUY2,width={WIDTH},height={HEIGHT},framerate={FPS}/1 '
    f'! videoconvert '
    f'! gtksink name=sink sync=false'
)

KEYMAP_HELP = """\
=== mini2 thermal controls (works in terminal OR video window) ===
  f         trigger FFC (flat-field correction)
  c         cycle colormap (0..11)
  m         cycle scene_mode (0..5)
  a         toggle auto_shutter
  y         toggle output_mode (YUV / Y16)
  + / -     brightness +/- 10
  ] / [     contrast +/- 5
  } / {     digital_detail_enhancement +/- 5
  ESC, q    quit
  Ctrl+C    quit (from launching terminal)
"""


# ---------------------------------------------------------------------------
# v4l2 control helpers
# ---------------------------------------------------------------------------

def get_ctrl(name):
    result = _run_v4l2(['v4l2-ctl', '--get-ctrl', name, '-d', SUBDEV])
    if result.returncode != 0:
        raise subprocess.CalledProcessError(result.returncode, result.args)
    return int(result.stdout.split(':', 1)[1].strip())


def set_ctrl(name, value):
    _run_v4l2(['v4l2-ctl', f'--set-ctrl={name}={value}', '-d', SUBDEV])
    print(f'{name}={value}', flush=True)


def cycle_menu(name, max_value):
    set_ctrl(name, (get_ctrl(name) + 1) % (max_value + 1))


def toggle_bool(name):
    set_ctrl(name, 1 - get_ctrl(name))


def bump(name, delta, lo, hi):
    set_ctrl(name, max(lo, min(hi, get_ctrl(name) + delta)))


def fire_ffc():
    set_ctrl('ffc_trigger', 1)
    return False  # GLib timeout one-shot


def quit_app(*_):
    Gtk.main_quit()
    return False


# ---------------------------------------------------------------------------
# Key dispatch
# ---------------------------------------------------------------------------

# Single source of truth for what each named action does. Both the Gdk
# key-press handler (video window) and the stdin reader (terminal) dispatch
# through this dict.
ACTIONS = {
    'ffc':          lambda: set_ctrl('ffc_trigger', 1),
    'colormap':     lambda: cycle_menu('colormap', 11),
    'scene':        lambda: cycle_menu('scene_mode', 5),
    'auto_shutter': lambda: toggle_bool('auto_shutter'),
    'output_mode':  lambda: toggle_bool('output_mode'),
    'bright_up':    lambda: bump('brightness',  10, 0, 100),
    'bright_dn':    lambda: bump('brightness', -10, 0, 100),
    'contrast_up':  lambda: bump('contrast',  5, 0, 100),
    'contrast_dn':  lambda: bump('contrast', -5, 0, 100),
    'dde_up':       lambda: bump('digital_detail_enhancement',  5, 0, 100),
    'dde_dn':       lambda: bump('digital_detail_enhancement', -5, 0, 100),
    'quit':         quit_app,
}

GDK_KEY_TO_ACTION = {
    Gdk.KEY_f: 'ffc', Gdk.KEY_F: 'ffc',
    Gdk.KEY_c: 'colormap', Gdk.KEY_C: 'colormap',
    Gdk.KEY_m: 'scene', Gdk.KEY_M: 'scene',
    Gdk.KEY_a: 'auto_shutter', Gdk.KEY_A: 'auto_shutter',
    Gdk.KEY_y: 'output_mode', Gdk.KEY_Y: 'output_mode',
    Gdk.KEY_plus: 'bright_up', Gdk.KEY_equal: 'bright_up',
    Gdk.KEY_minus: 'bright_dn', Gdk.KEY_underscore: 'bright_dn',
    Gdk.KEY_bracketright: 'contrast_up',
    Gdk.KEY_bracketleft:  'contrast_dn',
    Gdk.KEY_braceright:   'dde_up',
    Gdk.KEY_braceleft:    'dde_dn',
    Gdk.KEY_Escape: 'quit',
    Gdk.KEY_q: 'quit', Gdk.KEY_Q: 'quit',
}

CHAR_TO_ACTION = {
    'f': 'ffc', 'F': 'ffc',
    'c': 'colormap', 'C': 'colormap',
    'm': 'scene', 'M': 'scene',
    'a': 'auto_shutter', 'A': 'auto_shutter',
    'y': 'output_mode', 'Y': 'output_mode',
    '+': 'bright_up', '=': 'bright_up',
    '-': 'bright_dn', '_': 'bright_dn',
    ']': 'contrast_up',
    '[': 'contrast_dn',
    '}': 'dde_up',
    '{': 'dde_dn',
    'q': 'quit', 'Q': 'quit',
    '\x03': 'quit',  # Ctrl+C fallback if ISIG ever gets disabled
}


def on_gdk_key(_widget, event):
    name = GDK_KEY_TO_ACTION.get(event.keyval)
    if name is not None:
        ACTIONS[name]()
    return True


_old_termios = None


def _restore_termios():
    global _old_termios
    if _old_termios is None:
        return
    try:
        termios.tcsetattr(sys.stdin.fileno(),
                          termios.TCSADRAIN, _old_termios)
    except (termios.error, OSError):
        pass
    _old_termios = None


def _setup_stdin_reader():
    """Put stdin into cbreak mode and watch fd 0 with the GLib main loop, so
    keys typed into the launching terminal dispatch through ACTIONS the same
    way as keys pressed on the fullscreen video window.

    Skipped silently when stdin is not a tty (e.g., ssh without -t, or
    launched from a desktop file). The Gdk path still works in that case."""
    global _old_termios
    if not sys.stdin.isatty():
        return
    fd = sys.stdin.fileno()
    _old_termios = termios.tcgetattr(fd)
    tty.setcbreak(fd)        # ICANON off, ECHO off, ISIG kept on (Ctrl+C still SIGINT)
    atexit.register(_restore_termios)

    def on_input(_source, _condition):
        try:
            data = os.read(fd, 64)
        except OSError:
            return True
        if not data:
            return True
        i = 0
        while i < len(data):
            b = data[i]
            if b == 0x1b:
                # ESC vs CSI/SS3 escape sequence (e.g. arrow keys \x1b[A).
                # If the next byte in this same read is '[' or 'O', drain
                # the sequence; otherwise treat as bare ESC -> quit.
                if i + 1 < len(data) and data[i + 1] in (0x5b, 0x4f):
                    i += 2
                    while i < len(data) and not (0x40 <= data[i] <= 0x7e):
                        i += 1
                    i += 1  # past terminator byte
                    continue
                ACTIONS['quit']()
                return False
            ch = chr(b) if b < 128 else ''
            name = CHAR_TO_ACTION.get(ch)
            if name is not None:
                ACTIONS[name]()
            i += 1
        return True

    GLib.io_add_watch(fd, GLib.IO_IN | GLib.IO_HUP, on_input)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    Gst.init(None)
    try:
        pipeline = Gst.parse_launch(PIPELINE)
    except GLib.Error as exc:
        print(f'pipeline parse failed: {exc}', file=sys.stderr)
        print('check that gstreamer1.0-gtk3 is installed (provides gtksink)',
              file=sys.stderr)
        sys.exit(1)

    sink = pipeline.get_by_name('sink')
    video_widget = sink.props.widget

    window = Gtk.Window(title=f'mini2 thermal {WIDTH}x{HEIGHT}')
    window.set_default_size(WIDTH * 2, HEIGHT * 2)
    window.add(video_widget)
    window.set_can_focus(True)
    window.connect('destroy', quit_app)
    window.connect('key-press-event', on_gdk_key)

    # Schedule FFC for 2s after the FIRST frame flows through v4l2src.
    # Earlier triggers (e.g. 2s after script launch, or even at PAUSED->PLAYING
    # state change) hit the sensor before it has produced any frames and the
    # rs300 driver returns EIO on the I2C ffc command.
    src_element = pipeline.get_by_name('src')
    src_pad = src_element.get_static_pad('src')

    def _on_first_buffer(pad, _info):
        _log('first buffer from v4l2src — arming FFC for +2s')
        GLib.timeout_add_seconds(2, fire_ffc)
        return Gst.PadProbeReturn.REMOVE  # one-shot

    src_pad.add_probe(Gst.PadProbeType.BUFFER, _on_first_buffer)

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, quit_app)

    _setup_stdin_reader()

    print(KEYMAP_HELP, flush=True)

    pipeline.set_state(Gst.State.PLAYING)
    window.show_all()
    window.fullscreen()

    gdk_window = window.get_window()
    if gdk_window is not None:
        blank = Gdk.Cursor.new_for_display(
            Gdk.Display.get_default(), Gdk.CursorType.BLANK_CURSOR
        )
        gdk_window.set_cursor(blank)

    try:
        Gtk.main()
    finally:
        pipeline.set_state(Gst.State.NULL)
        _restore_termios()

    print('stream stopped.', flush=True)


if __name__ == '__main__':
    main()
