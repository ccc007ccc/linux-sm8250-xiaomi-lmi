# lmi-camera Rust runtime prototype

This is the Rust control-plane prototype for the lmi local camera path.

Primary target:

```text
/dev/video3 RAW pgAA -> software ISP -> /dev/video20 v4l2loopback -> local Linux camera apps
```

UVC is a secondary optional backend.

Current commands are diagnostic/control-plane prototype commands:

```bash
cargo run -- probe
cargo run -- media
cargo run -- route-summary
cargo run -- formats /dev/video3
cargo run -- route-check
cargo run -- setup-route --size 1364x768 --reset-controls
cargo run -- capture-raw --frames 30 --sink null --ctrl /dev/v4l-subdev5 --exposure 120
cargo run -- profile
cargo run -- isp-command
cargo run -- run --route-size 1364x768 --preserve-controls
cargo run -- pipeline-plan
```

`setup-route`, `capture-raw`, and `run` accept the OV13B10 sensor-control options `--vblank`, `--exposure`, `--analogue-gain`, `--digital-gain`, `--reset-controls`, and `--preserve-controls`. `capture-raw` and `run` can also take an explicit `--ctrl DEV` when route setup is skipped or the control subdev is known in advance.

The initial `run` command configures the fixed OV13B10 RAW route, then supervises the existing C `lmi-isp` local-loopback backend. It is still an early runtime replacement because the image-processing data plane remains in C.

The first implementation intentionally does not rewrite `lmi-isp.c`. It starts by moving device discovery, camera invariants, media-route setup, sensor-control setup, bounded RAW capture smoke testing, local-loopback profile generation, and C ISP supervision into Rust.
