"""One-Euro filter (Casiez et al. 2012), constant-velocity Kalman, and the pose predictor."""
import math
import numpy as np


class OneEuro:
    def __init__(self, mincutoff=1.0, beta=0.02, dcutoff=1.0):
        self.mincutoff, self.beta, self.dcutoff = mincutoff, beta, dcutoff
        self.x = None
        self.dx = None
        self.t = None

    @staticmethod
    def _alpha(cutoff, dt):
        tau = 1.0 / (2 * math.pi * cutoff)
        return 1.0 / (1.0 + tau / dt)

    def __call__(self, x, t):
        x = np.asarray(x, np.float64)
        if self.x is None or self.x.shape != x.shape:
            self.x, self.dx, self.t = x.copy(), np.zeros_like(x), t
            return x
        dt = max(t - self.t, 1e-3)
        self.t = t
        dx = (x - self.x) / dt
        a_d = self._alpha(self.dcutoff, dt)
        self.dx = a_d * dx + (1 - a_d) * self.dx
        cutoff = self.mincutoff + self.beta * np.abs(self.dx)
        a = 1.0 / (1.0 + (1.0 / (2 * np.pi * cutoff)) / dt)
        self.x = a * x + (1 - a) * self.x
        return self.x.copy()

    def reset(self):
        self.x = None


class KalmanCV:
    """Constant-velocity Kalman filter, independent per element (vectorised).

    White-noise-acceleration process model with std `accel_std` (m/s^2). Unlike a low-pass
    filter it has no steady-state lag on constant-velocity motion, and it estimates velocity,
    which is what pose prediction extrapolates with.
    """

    def __init__(self, accel_std=25.0, meas_std=0.004, jump=0.5):
        self.q, self.r, self.jump = accel_std ** 2, meas_std, jump
        self.p = None

    def __call__(self, z, t, meas_std=None):
        z = np.asarray(z, np.float64)
        R = np.broadcast_to(np.asarray(self.r if meas_std is None else meas_std, np.float64) ** 2, z.shape)
        if self.p is None or self.p.shape != z.shape:
            self.p, self.v, self.t = z.copy(), np.zeros_like(z), t
            self.P00, self.P01, self.P11 = R.copy(), np.zeros_like(z), np.ones_like(z)
            return self.p.copy(), self.v.copy()
        dt = min(max(t - self.t, 1e-3), 0.5)
        self.t = t
        q = self.q
        self.p = self.p + self.v * dt
        P00 = self.P00 + dt * (2 * self.P01 + dt * self.P11) + q * dt ** 4 / 4
        P01 = self.P01 + dt * self.P11 + q * dt ** 3 / 2
        P11 = self.P11 + q * dt ** 2
        S = P00 + R
        K0, K1 = P00 / S, P01 / S
        y = z - self.p
        self.p = self.p + K0 * y
        self.v = self.v + K1 * y
        self.P00, self.P01, self.P11 = (1 - K0) * P00, (1 - K0) * P01, P11 - K1 * P01
        bad = np.abs(y) > self.jump                  # no body part moves this far in one frame:
        if bad.any():                                # a glitch or re-detection, restart there
            self.p[bad], self.v[bad] = z[bad], 0.0
            self.P00[bad], self.P01[bad], self.P11[bad] = R[bad], 0.0, 1.0
        return self.p.copy(), self.v.copy()

    def reset(self):
        self.p = None


class PosePredictor:
    """Filtered position + velocity per point, for render-time extrapolation in Unity.

    At rest the One-Euro output is used and velocity is zero (no jitter, nothing to
    extrapolate); once a point moves, it switches to a fast Kalman estimate, whose velocity
    the renderer extrapolates to the moment the frame is on screen. The switch is driven by
    the speed from a heavily smoothed Kalman filter, because the fast filter's velocity is too
    noisy at rest (0.2-0.5 m/s from ~2 px of landmark noise at 30 Hz). Tuned end to end
    (filter -> TCP -> TrackingClient) on a known trajectory with 60 ms latency: vs showing the
    latest filtered pose, ~50 % less error in fast motion, ~20 % in slow, rest jitter < 1 mm.

    Input (N, 3) or (3,) in metres; returns (pos, vel) of the same shape.
    """

    def __init__(self, mincutoff=1.2, beta=0.8, accel_std=25.0, gate_accel_std=2.0,
                 gate_speed=(0.1, 0.35), meas_std=(0.004, 0.004, 0.02), max_speed=4.0):
        self.meas = np.asarray(meas_std, np.float64)
        self.oe = OneEuro(mincutoff, beta)
        self.fast = KalmanCV(accel_std, self.meas)
        self.slow = KalmanCV(gate_accel_std, self.meas)
        self.lo, self.hi = gate_speed
        self.max_speed = max_speed
        self.gate = None

    def __call__(self, x, t, vis=None):
        x = np.asarray(x, np.float64)
        r = self.meas
        if vis is not None:                          # occluded / uncertain points: trust less
            r = r / np.clip(np.asarray(vis, np.float64), 0.2, 1.0)[..., None]
        smooth = self.oe(x, t)
        p, v = self.fast(x, t, r)
        _, vs = self.slow(x, t, r)
        # z is ~5x noisier than x/y, so count it at that discount
        speed = np.hypot(vs[..., 0], vs[..., 1]) + np.abs(vs[..., 2]) * self.meas[0] / self.meas[2]
        g = np.clip((speed - self.lo) / (self.hi - self.lo), 0.0, 1.0)
        g = (g * g * (3 - 2 * g))[..., None]
        self.gate = g[..., 0]
        n = np.linalg.norm(v, axis=-1, keepdims=True)
        v = v * np.minimum(1.0, self.max_speed / np.maximum(n, 1e-9))
        return smooth + g * (p - smooth), g * v

    def reset(self):
        self.oe.reset(); self.fast.reset(); self.slow.reset()
