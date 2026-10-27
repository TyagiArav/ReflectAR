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
# D415 limits: colour reaches 60 fps at 960x540 and below (30 at 1280x720), depth 90 fps at
# 848x480 and below (30 at 1280x720). On USB 3 we take the fast modes; a USB 2 link (often just
# a USB 2 cable) only delivers 640x480 colour + depth at 15 fps: faster combos start but send nothing.
MODES = {"usb3": {"color": [960, 540], "color_fps": 60, "depth": [848, 480], "depth_fps": 90},
         "usb2": {"color": [640, 480], "color_fps": 15, "depth": [640, 480], "depth_fps": 15}}


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


def _pick(dev, stream, fmt, w, h, fps):
    """The requested mode if the camera offers it, else the fastest offered one no larger than
    it, so a config typo can't cause a refused start (which leaves the camera stuck)."""
    offered = {(p.width(), p.height(), p.fps())
               for s in dev.query_sensors() for p in s.get_stream_profiles()
               if p.stream_type() == stream and p.format() == fmt and p.is_video_stream_profile()
               for p in [p.as_video_stream_profile()]}
    if (w, h, fps) in offered:
        return w, h, fps
    fits = [m for m in offered if m[0] <= w and m[1] <= h and m[2] <= fps]
    if not fits:
        raise RuntimeError(f"RealSense offers no {stream} mode up to {w}x{h} @ {fps}")
    best = max(fits, key=lambda m: (m[2], m[0] * m[1]))
    print(f"[realsense] {stream} {w}x{h} @ {fps} isn't offered; using {best[0]}x{best[1]} @ {best[2]}")
    return best


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
        # librealsense segfaults while tearing down at interpreter exit, even after a failed
        # query: from here on, stop the streams and leave without running its destructors (a
        # segfault there leaves the camera too stuck for the next start's reset)
        self.pipe = None
        atexit.register(self._exit)
        # On macOS the start is a race with the system camera service over the colour interface,
        # and a camera left by a stopped or crashed run is half-claimed: a query can fail with
        # "failed to set power state", or the start succeeds and never sends a frame. Everything
        # touching the device happens after a reset, and any failure costs one attempt.
        tries, link = rc.get("start_tries", 4), "usb3"
        for attempt in range(1, tries + 1):
            try:
                if rc.get("reset_on_start", True) or attempt > 1:
                    devs = _reset(devs[0]) if len(devs) else rs.context().query_devices()
                dev = devs[0]
                usb = dev.get_info(rs.camera_info.usb_type_descriptor) \
                    if dev.supports(rs.camera_info.usb_type_descriptor) else "3"
                link = "usb3" if usb.startswith("3") else "usb2"
                m = {**MODES[link], **rc.get(link, {})}
                if "fps" in m:                           # one rate for both streams
                    m["color_fps"] = m["depth_fps"] = m["fps"]
                cw, ch, cf = _pick(dev, rs.stream.color, rs.format.bgr8, *m["color"], m["color_fps"])
                dw, dh, df = _pick(dev, rs.stream.depth, rs.format.z16, *m["depth"], m["depth_fps"])
                self._set_options(dev.first_depth_sensor(), rc.get("preset"))  # before streaming: USB 2
                cfg = rs.config()                                                # control transfers stall mid-stream
                cfg.enable_device(dev.get_info(rs.camera_info.serial_number))
                cfg.enable_stream(rs.stream.color, cw, ch, rs.format.bgr8, cf)
                cfg.enable_stream(rs.stream.depth, dw, dh, rs.format.z16, df)
            except (RuntimeError, IndexError) as e:          # IndexError: not back after a reset yet
                err = f"can't access the camera ({e or 'not connected'})"
                print(f"[realsense] attempt {attempt}/{tries}: {err}")
                time.sleep(2.0)
                devs = rs.context().query_devices()
                continue
            self.pipe = rs.pipeline()
            try:
                prof = self.pipe.start(cfg)
            except RuntimeError as e:
                err = f"refused colour {cw}x{ch} @ {cf} + depth {dw}x{dh} @ {df} fps ({e})"
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
            raise RuntimeError(f"RealSense: {err}. Replug it (and quit any app using it as a "
                               f"webcam), or change camera.realsense.{link} in config.json")
        dev = prof.get_device()                            # the pre-start handle can't be used once streaming
        self.depth_scale = dev.first_depth_sensor().get_depth_scale()
        ci = prof.get_stream(rs.stream.color).as_video_stream_profile().get_intrinsics()
        self.intrinsics = dict(fx=ci.fx, fy=ci.fy, cx=ci.ppx, cy=ci.ppy, w=ci.width, h=ci.height)
        self.usb = usb
        self.desc = (f"{dev.get_info(rs.camera_info.name)} fw {dev.get_info(rs.camera_info.firmware_version)} "
                     f"USB {self.usb}, colour {ci.width}x{ci.height} @ {cf} fps, depth {dw}x{dh} @ {df} fps")
        self.align = rs.align(rs.stream.color)
        self.frame, self.depth, self.stamp, self.seq = None, None, 0.0, 0
        self._depth_next = None
        self.depth_hz = self.color_hz = 0.0              # frames actually arriving
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
        last_c = last_d = -1
        n_d, n_c, t_hz = 0, 0, time.time()
        while True:
            try:
                fs = self.pipe.wait_for_frames(5000)    # the first frames take ~2 s after start
            except RuntimeError:                       # stalled / unplugged: keep trying
                print("[realsense] no frames for 5 s")
                continue
            t_recv = time.time()
            # With depth faster than colour the pipeline hands over a set per depth frame, holding
            # the last colour frame: only a new colour frame is a new frame for tracking, and it
            # comes paired with the newest depth (at 90 fps at most ~11 ms older)
            d_raw, c = fs.get_depth_frame(), fs.get_color_frame()
            if d_raw and d_raw.get_frame_number() != last_d:
                last_d = d_raw.get_frame_number()
                n_d += 1
            if t_recv - t_hz > 1.0:
                dt = t_recv - t_hz
                self.depth_hz, self.color_hz, n_d, n_c, t_hz = n_d / dt, n_c / dt, 0, 0, t_recv
            if not c or not d_raw or c.get_frame_number() == last_c:
                continue
            last_c = c.get_frame_number()
            n_c += 1
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

    @property
    def hz(self):
        return self.cam.depth_hz

    def submit(self, _bgr):                            # depth arrives with the frame
        pass

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
