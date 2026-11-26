# PN532 zero-length frame regression (two-sided, checked-in)

`pn532-zerolen-frame` feeds `00 FF 00 00 00 00` through a POSIX PTY into the
real `SerialPort` and `readFrame()` (the driver source is compiled into the test
translation unit because `readFrame()` is file-local). It asserts
`FrameKind::None` and an empty payload. Before the zero-length guard the parser
called `at(0)` on an empty payload: an assert-enabled build aborts, an
assert-free build reports a bogus `Data` frame. Both fail this case.

`pn532-zerolen-controls` covers ACK, NACK, valid data (protocol-correct DCS),
bad LCS, truncation, extended-length desync, Error frame, and the literal
wrong-DCS data frame. Those cases pass on both sides, so a failure of the gate
case is attributable to the zero-length defect.

`QT_FORCE_ASSERTS` is a compile-time requirement of this target; there is no
skip path for assert-enabled builds.

## Run

```sh
cmake -B build-test -DBUILD_TESTS=ON
cmake --build build-test --target pn532-zerolen-frame-tests -j4
ctest --test-dir build-test -R '^pn532-zerolen' --output-on-failure
```

## Replay the RED arm after the guard lands

Point `PN532_SOURCE_ROOT` at a pinned pre-fix checkout and use a separate build
directory, so objects compiled against the two source variants cannot mix:

```sh
cmake -B build-ctl -DBUILD_TESTS=ON -DPN532_SOURCE_ROOT=/path/to/pre-fix/tree
cmake --build build-ctl --target pn532-zerolen-frame-tests -j4
ctest --test-dir build-ctl -R '^pn532-zerolen-frame$' --output-on-failure
```

The gate case must abort with a `QByteArray::at()` assertion; the controls must
still pass.
