"""Intel RealSense (D415) capture: colour frames for tracking plus depth aligned to them.

Replaces the webcam + Depth Anything pair when a RealSense is plugged in: the colour stream
feeds BlazePose exactly like a webcam frame, and every frame comes with a metric depth map
registered to the colour pixels, so the distance (and each joint's depth) is measured instead
of estimated. Intrinsics come from the device's factory calibration.

macOS: librealsense talks to the camera over libusb, which macOS only allows for root
("failed to set power state" otherwise), so start the service with sudo there.
"""
import atexit, os, sys, threading, time
import numpy as np

try:
    import pyrealsense2 as rs
except ImportError:                                  # optional: webcam mode works without it
    rs = None

# One mode per link type, started once: after a refused mode the D415 stays half-claimed
# ("failed to claim usb interface") and the next attempt fails or crashes, so no trial and error.
# A D415 on USB 3 runs 1280x720 colour + depth at 30 fps; a USB 2 link (often just a USB 2
# cable) can't carry that, and 640x480 colour + depth only arrives at 15 fps.
MODES = {"usb3": {"color": [1280, 720], "depth": [1280, 720], "fps": 30},
         "usb2": {"color": [640, 480], "depth": [640, 480], "fps": 15}}


def available():
    """True when pyrealsense2 is installed and a device is connected that we can open."""
    if rs is None:
        return False
    try:
        if len(rs.context().query_devices()) == 0:
            return False
    except RuntimeError as e:
        print(f"[realsense] {e}")
        return False
    if not _can_open():
        print("[realsense] a RealSense is connected but macOS only lets root open it: "
              "start the service with sudo to use it. Using the webcam.")
        return False
    return True


def _can_open():
    # Touching the device without USB access fails with "failed to set power state" and then
    # segfaults librealsense when the half-opened device is released, so don't try.
    return sys.platform != "darwin" or os.geteuid() == 0


def _reset(dev, timeout=15.0):
    """Power-cycle the camera and wait for it to come back. A D415 that was stopped, killed or
    refused a mode often starts streaming again but never delivers a frame; a reset clears it."""
    print("[realsense] resetting the camera...")
    dev.hardware_reset()
    t0, gone = time.time(), False
    while time.time() - t0 < timeout:
        time.sleep(0.5)
        devs = rs.context().query_devices()
        gone = gone or len(devs) == 0
        if gone and len(devs) > 0:
            time.sleep(2.0)                              # let it finish enumerating
            return rs.context().query_devices()
    raise RuntimeError("RealSense didn't come back after a reset: replug it")


class RealSenseCamera:
    """Same interface as server.Camera (read(last_seq) -> frame, stamp, seq) plus `depth`:
    the depth map (uint16, `depth_scale` metres per unit, 0 = no data) of the frame that
    read() last returned, aligned to its pixels."""

    def __init__(self, rc=None):
        """rc: the config's camera.realsense section (usb3 / usb2 modes, preset)."""
        rc = rc or {}
        if rs is None:
            raise RuntimeError("pyrealsense2 is not installed: .venv/bin/pip install -r requirements.txt")
        devs = rs.context().query_devices()
        if len(devs) == 0:
            raise RuntimeError("no RealSense connected")
        if not _can_open():
            raise RuntimeError("macOS only lets root open a RealSense: start the service with sudo")
        usb = devs[0].get_info(rs.camera_info.usb_type_descriptor) \
            if devs[0].supports(rs.camera_info.usb_type_descriptor) else "3"
        link = "usb3" if usb.startswith("3") else "usb2"
        m = {**MODES[link], **rc.get(link, {})}
        (cw, ch), (dw, dh), f = m["color"], m["depth"], m["fps"]
        # librealsense segfaults while tearing down at interpreter exit (even after a refused
        # start): stop the streams, then leave without running its destructors
        self.pipe = None
        atexit.register(self._exit)
        # On macOS the start is a race with the system camera service over the colour interface:
        # sometimes the start is refused ("failed to set power state") or it starts and never
        # sends a frame. A reset and another go usually wins it.
        tries = rc.get("start_tries", 4)
        for attempt in range(1, tries + 1):
            if rc.get("reset_on_start", True) or attempt > 1:
                devs = _reset(devs[0])
            dev = devs[0]
            sensor = dev.first_depth_sensor()
            self._set_options(sensor, rc.get("preset"))   # before streaming: USB 2 control transfers stall mid-stream
            cfg = rs.config()
            cfg.enable_device(dev.get_info(rs.camera_info.serial_number))
            cfg.enable_stream(rs.stream.color, cw, ch, rs.format.bgr8, f)
            cfg.enable_stream(rs.stream.depth, dw, dh, rs.format.z16, f)
            self.pipe = rs.pipeline()
            try:
                prof = self.pipe.start(cfg)
            except RuntimeError as e:
                err = f"refused colour {cw}x{ch} + depth {dw}x{dh} @ {f} fps ({e})"
                self.pipe = None
            else:
                try:
                    self.pipe.wait_for_frames(8000)
                    break
                except RuntimeError:
                    err = "started but sent no frames"
                    self.pipe.stop()
                    self.pipe = None
            print(f"[realsense] attempt {attempt}/{tries}: {err}")
        else:
            raise RuntimeError(f"RealSense on USB {usb} {err}. Replug it (and quit any app using it as a "
                               f"webcam), or change camera.realsense.{link} in config.json")
        dev = prof.get_device()                            # the pre-start handle can't be used once streaming
        self.depth_scale = dev.first_depth_sensor().get_depth_scale()
        ci = prof.get_stream(rs.stream.color).as_video_stream_profile().get_intrinsics()
        self.intrinsics = dict(fx=ci.fx, fy=ci.fy, cx=ci.ppx, cy=ci.ppy, w=ci.width, h=ci.height)
        self.usb = usb
        self.desc = (f"{dev.get_info(rs.camera_info.name)} fw {dev.get_info(rs.camera_info.firmware_version)} "
                     f"USB {self.usb}, colour {ci.width}x{ci.height}, depth {dw}x{dh} @ {f} fps")
        self.align = rs.align(rs.stream.color)
        self.frame, self.depth, self.stamp, self.seq = None, None, 0.0, 0
        self._depth_next = None
        self.cond = threading.Condition()
        threading.Thread(target=self._loop, daemon=True).start()

    @staticmethod
    def _set_options(sensor, preset):
        if preset and sensor.supports(rs.option.visual_preset):
            for i in range(int(sensor.get_option_range(rs.option.visual_preset).max) + 1):
                if sensor.get_option_value_description(rs.option.visual_preset, i).lower() == preset.lower():
                    sensor.set_option(rs.option.visual_preset, i)
        if sensor.supports(rs.option.global_time_enabled):
            sensor.set_option(rs.option.global_time_enabled, 1)

    def _exit(self):
        try:
            if self.pipe is not None:
                self.pipe.stop()
        except RuntimeError:
            pass
        sys.stdout.flush(); sys.stderr.flush()
        os._exit(0)

    def _loop(self):
        while True:
            try:
                fs = self.pipe.wait_for_frames(5000)    # the first frames take ~2 s after start
            except RuntimeError:                       # stalled / unplugged: keep trying
                print("[realsense] no frames for 5 s")
                continue
            t_recv = time.time()
            fs = self.align.process(fs)
            c, d = fs.get_color_frame(), fs.get_depth_frame()
            if not c or not d:
                continue
            # global-time timestamps are mid-exposure on the host clock: better than receive time
            t = c.get_timestamp() / 1000.0
            if c.get_frame_timestamp_domain() != rs.timestamp_domain.global_time or abs(t - t_recv) > 0.5:
                t = t_recv
            color = np.asanyarray(c.get_data()).copy()
            depth = np.asanyarray(d.get_data()).copy()
            with self.cond:
                self.frame, self._depth_next, self.stamp, self.seq = color, depth, t, self.seq + 1
                self.cond.notify_all()

    def read(self, last_seq):
        with self.cond:
            # the first read waits for the stream to come up; later ones like server.Camera
            if not self.cond.wait_for(lambda: self.seq != last_seq, timeout=1.0 if self.seq else 10.0) \
                    and self.frame is None:
                raise RuntimeError("RealSense started but sent no frames: replug it, or pick a smaller "
                                   "mode in config.json (camera.realsense)")
            self.depth = self._depth_next
            return self.frame, self.stamp, self.seq


class SensorDepth:
    """Drop-in for depth.MonoDepth backed by the camera's measured depth (metres)."""

    def __init__(self, cam: RealSenseCamera):
        self.cam = cam
        self.active = True
        self.hz = 0.0
        self._last = time.time()

    def submit(self, _bgr):                            # depth arrives with the frame
        t = time.time()
        self.hz = 0.9 * self.hz + 0.1 / max(t - self._last, 1e-3)
        self._last = t

    def window(self, u, v, img_w, img_h, r=6):
        """Valid depths (m) in a (2r+1)^2 window around frame pixel (u, v)."""
        d = self.cam.depth
        if d is None:
            return np.empty(0)
        H, W = d.shape
        x = int(np.clip(u * W / img_w, 0, W - 1)); y = int(np.clip(v * H / img_h, 0, H - 1))
        win = d[max(y - r, 0):y + r + 1, max(x - r, 0):x + r + 1]
        return win[win > 0] * self.cam.depth_scale

    def sample(self, u, v, img_w, img_h, r=6):
        """Median depth around frame pixel (u, v), like MonoDepth.sample."""
        win = self.window(u, v, img_w, img_h, r)
        win = win[(win > 0.2) & (win < 8.0)]
        return float(np.median(win)) if win.size else None

    def joint(self, u, v, img_w, img_h, z_guess, band=0.35, r=5):
        """Body-surface depth at a joint: the nearest surface within `band` m of z_guess, so a
        window straddling a limb's edge reads the limb rather than the wall behind it."""
        win = self.window(u, v, img_w, img_h, r)
        win = win[np.abs(win - z_guess) < band]
        return float(np.percentile(win, 25)) if win.size >= 4 else None
