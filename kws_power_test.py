#!/usr/bin/env python3
"""Run a pretrained KWS model on the live AKD1000 and read MEASURED power.

Loads DS-CNN keyword-spotting (akida-models), maps it to the hardware device,
enables the onboard power meter, runs N inferences, and reports measured power +
per-inference energy + framerate. Requires the akida-pcie driver loaded + a
writable /dev/akida0. Self-diagnosing: prints available APIs if a call differs.
"""
import time
import numpy as np
import akida


def load_kws_model():
    """Fetch the Akida-1.0 (4-bit) DS-CNN KWS model and convert it for the AKD1000.

    The AKD1000 is Akida 1.0 (IP BC.00.000.002, 4-bit max). The default pretrained
    is an AkidaV2 8-bit model that won't map; we target v1 + bitwidth=4, which
    fetches ds_cnn_kws_iq8_wq4_aq4_laq1.h5 (the v1/4-bit model).
    """
    import akida_models
    from cnn2snn import convert, set_akida_version, AkidaVersion
    with set_akida_version(AkidaVersion.v1):
        print("[model] ds_cnn_kws_pretrained(bitwidth=4) in AkidaVersion.v1 context")
        m = akida_models.ds_cnn_kws_pretrained(bitwidth=4)
        return m if isinstance(m, akida.Model) else convert(m)


def main():
    devs = akida.devices()
    if not devs:
        raise SystemExit("no akida device — akida-pcie loaded? /dev/akida0 present+writable?")
    dev = devs[0]
    print("[device]", dev, "version:", dev.version)

    model = load_kws_model()
    ishape = tuple(model.input_shape)
    print("[model] input_shape:", ishape)

    try:
        dev.soc.power_measurement_enabled = True
        print("[power] measurement enabled; floor:", dev.soc.power_meter.floor, "mW")
    except Exception as e:
        print("[power] enable/floor err:", e)

    model.map(dev)
    print("[map] model mapped to hardware")

    N = 200
    x = np.random.randint(0, 256, size=(N,) + ishape, dtype=np.uint8)
    t0 = time.time()
    try:
        out = model.forward(x)
    except AttributeError:
        out = model.predict(x)
    dt = time.time() - t0
    print(f"[infer] {N} inferences in {dt:.3f}s -> {N/dt:.1f} inf/s, out={getattr(out,'shape',type(out))}")

    print("=== model.statistics ===")
    print(model.statistics)

    pm = dev.soc.power_meter
    print("[power] floor:", pm.floor, "mW")
    evts = list(pm.events())  # PowerEvents captured during inference
    print("[power] event count:", len(evts))
    if evts:
        print("[power] event attrs:", [a for a in dir(evts[0]) if not a.startswith("_")])
        ps = [getattr(e, "power", None) for e in evts]
        ps = [p for p in ps if p is not None]
        if ps:
            print(f"[power] inference power: n={len(ps)} mean={sum(ps)/len(ps):.2f} mW "
                  f"min={min(ps):.2f} max={max(ps):.2f} mW")


if __name__ == "__main__":
    main()
