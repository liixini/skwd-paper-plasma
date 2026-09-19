# Plasma checks

Run `scripts/test-all.sh` with `TMPDIR` pointing to a short disk-backed directory.
The Python tests guard source ownership and packaging contracts. They do not
establish that a renderer displayed the requested wallpaper.

The C++ tests exercise the real monitor socket protocol, presenter lifecycle and
descriptor ownership. The CPU presentation test feeds controlled frames through
the item and checks the pixels visible in the Qt Quick window, including a changed
frame. Set `SKWD_TEST_ARTIFACT_DIR` to retain those captures. Presenter process
doubles test bridge decisions; they do not establish Paper's control semantics.

For compositor coverage, build with testing enabled and run:

```sh
python3 scripts/test-presentation-live.py \
  --build /path/to/build \
  --paper /path/to/suite/bin/skwd-paper \
  --results /path/to/new/results \
  --runtime /short/new/runtime
```

This creates a private KWin session and stages the candidate plugin. A click
changes the same monitor's connector, then image assertions verify the old
assignment, the new connector's configured fallback and its delayed pushed
assignment. The test checks that the process loaded the staged plugin. Pass
`--plugin /path/to/baseline/plugin.so --monitor-only` to check an older artifact.

The GPU test uses a declared grayscale fixture to verify spatial content,
animation, pause/resume and one real Paper child PID for one or two output items.
Color conversion belongs to Paper; these grayscale checks do not establish its
YUV color accuracy. CPU frame checks separately verify primary colors. Captures,
logs and artifact hashes remain in the results directory.

The standalone `gpu-presentation-test PAPER ASSIGNMENTS_JSON ARTIFACT_DIR`
accepts existing assignment arrays. Supply an equally sized expectation array
through `SKWD_TEST_EXPECTATIONS_JSON`, or wrap each assignment in
`{"assignment": {...}, "expect": {...}}`. An expectation declares `pixels`
with normalized `x`, `y` and `color` fields, and may declare `animated: true`.
Every output must match the pixels; animated fixtures must change, hold still
when paused and change again after resume. Missing pixel oracles return 77,
which is an unavailable check, not a pass. `SKWD_TEST_SHARED_OUTPUTS` defaults
to one and uses the same pooled output path as Plasma.
