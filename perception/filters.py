"""One-Euro filter (Casiez et al. 2012), vectorised over any array shape."""
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
