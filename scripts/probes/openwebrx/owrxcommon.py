"""Shared by owrxsession.py and owrxanalyze.py: OpenWebRX's IMA ADPCM."""
import numpy as np

# ---- IMA ADPCM, as OpenWebRX's client decodes it ----
IDX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
        796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026,
        4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
        20350, 22385, 24623, 27086, 29794, 32767]

def adpcm_plain(data):
    """Per-frame decode from a reset state (the FFT path)."""
    st = {"i": 0, "p": 0, "s": 0}
    out = np.empty(len(data) * 2, np.int32)
    for n, b in enumerate(data):
        for h, nib in enumerate((b & 15, b >> 4)):
            st["i"] = min(max(st["i"] + IDX[nib], 0), 88)
            d = st["s"] >> 3
            if nib & 1: d += st["s"] >> 2
            if nib & 2: d += st["s"] >> 1
            if nib & 4: d += st["s"]
            if nib & 8: d = -d
            st["p"] = min(max(st["p"] + d, -32768), 32767)
            st["s"] = STEP[st["i"]]
            out[2 * n + h] = st["p"]
    return out


class AdpcmStereoSync:
    """The audio path's stereo IMA ADPCM with sync frames, as OpenWebRX's
    client decodes it: "SYNC", four int16 (step index and predictor of
    each channel), then 1 001 byte pairs (low nibble first; the first byte
    of a pair is channel 1, the second channel 2). State persists across
    messages."""

    def __init__(self):
        self.ch = [{"i": 0, "p": 0, "s": 0}, {"i": 0, "p": 0, "s": 0}]
        self.phase, self.matched, self.buf, self.counter = 0, 0, bytearray(), 0

    @staticmethod
    def _nib(st, nib):
        st["i"] = min(max(st["i"] + IDX[nib], 0), 88)
        d = st["s"] >> 3
        if nib & 1: d += st["s"] >> 2
        if nib & 2: d += st["s"] >> 1
        if nib & 4: d += st["s"]
        if nib & 8: d = -d
        st["p"] = min(max(st["p"] + d, -32768), 32767)
        st["s"] = STEP[st["i"]]
        return st["p"]

    def decode(self, data):
        a, b = [], []
        n, k = len(data), 0
        while k < n:
            x = data[k]
            if self.phase == 0:
                if x == b"SYNC"[self.matched]:
                    self.matched += 1
                else:
                    self.matched = 0
                if self.matched == 4:
                    self.phase, self.buf = 1, bytearray()
            elif self.phase == 1:
                self.buf.append(x)
                if len(self.buf) == 8:
                    v = np.frombuffer(bytes(self.buf), "<i2")
                    self.ch[0]["i"], self.ch[0]["p"] = int(v[0]), int(v[1])
                    self.ch[1]["i"], self.ch[1]["p"] = int(v[2]), int(v[3])
                    self.counter, self.phase = 1000, 2
            else:
                a.append(self._nib(self.ch[0], x & 15))
                a.append(self._nib(self.ch[0], x >> 4))
                k += 1
                y = data[k] if k < n else 0
                b.append(self._nib(self.ch[1], y & 15))
                b.append(self._nib(self.ch[1], y >> 4))
                if self.counter == 0:
                    self.matched, self.phase = 0, 0
                self.counter -= 1
            k += 1
        return np.array(a, np.int32), np.array(b, np.int32)
