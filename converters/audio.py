"""Sound files -> Ogg Vorbis, WAV or FLAC, with simple fixes on the way:
mono or stereo, another sample rate, even loudness, silence trimmed."""

from pathlib import Path

from forge_convert import ConvertError

FORMATS = {"ogg": ("OGG", "VORBIS", ".ogg"), "wav": ("WAV", "PCM_16", ".wav"), "flac": ("FLAC", "PCM_16", ".flac")}


def resample(data, rate, to):
    """Linear resampling, with a light low-pass when going down."""
    import numpy as np
    if rate == to:
        return data
    if to < rate:
        # Average over the step first, so high tones do not fold back.
        k = max(1, int(round(rate / to)))
        if k > 1:
            kernel = np.ones(k) / k
            data = np.stack([np.convolve(data[:, c], kernel, mode="same") for c in range(data.shape[1])], axis=1)
    n = int(round(len(data) * to / rate))
    x_old = np.arange(len(data)) / rate
    x_new = np.arange(n) / to
    return np.stack([np.interp(x_new, x_old, data[:, c]) for c in range(data.shape[1])], axis=1)


def convert(source, out, settings, report):
    try:
        import numpy as np
        import soundfile as sf
    except ImportError as e:
        raise ImportError(name=e.name or "soundfile") from e
    fmt = settings.get("format", "ogg")
    if fmt not in FORMATS:
        raise ConvertError(f"неизвестный формат «{fmt}»")
    report(0.05, "читаю " + source.name)
    try:
        data, rate = sf.read(str(source), dtype="float64", always_2d=True)
    except Exception as e:
        raise ConvertError(f"{source.name} не читается как звук: {e}")
    if len(data) == 0:
        raise ConvertError(f"в {source.name} нет звука")

    channels = settings.get("channels", "keep")
    if channels == "mono" and data.shape[1] > 1:
        data = data.mean(axis=1, keepdims=True)
    elif channels == "stereo" and data.shape[1] == 1:
        data = np.repeat(data, 2, axis=1)

    if settings.get("trim", False):
        report(0.3, "обрезаю тишину")
        loud = np.abs(data).max(axis=1) > 10 ** (-50 / 20)  # -50 dB
        idx = np.flatnonzero(loud)
        if len(idx):
            pad = int(rate * 0.01)
            data = data[max(0, idx[0] - pad):idx[-1] + 1 + pad]

    to = settings.get("rate", "keep")
    if to != "keep":
        report(0.5, f"перевожу в {to} Гц")
        data = resample(data, rate, int(to))
        rate = int(to)

    if settings.get("normalize", False):
        peak = np.abs(data).max()
        if peak > 0:
            data = data * (10 ** (-1 / 20) / peak)  # loudest point at -1 dB

    data = np.clip(data, -1.0, 1.0)
    container, subtype, ext = FORMATS[fmt]
    target = out / (source.stem + ext)
    report(0.8, "записываю " + target.name)
    kwargs = {}
    if fmt == "ogg":
        kwargs["compression_level"] = 1.0 - float(settings.get("quality", 0.6))
    try:
        sf.write(str(target), data, rate, format=container, subtype=subtype, **kwargs)
    except TypeError:  # an older soundfile without compression_level
        sf.write(str(target), data, rate, format=container, subtype=subtype)
    report(1, "готово")
    return [target]
