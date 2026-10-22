# Third-party notices

This directory distributes the prebuilt ARM `human-detect` program. The
project-owned source code and model data are BSD-3-Clause; see `LICENSE` and
`sdk/optional-src/human-detect/THIRD_PARTY_NOTICES.md`.

The executable statically includes these Apache-2.0 components:

- TensorFlow Lite Micro, revision
  `bfeb43aa795adad4044bbf2e709062b1b989a98a`;
- CMSIS-NN, revision `4ab83cc3cc98fb85ed6dafb55e8ca02f1628dcae`;
- FlatBuffers headers;
- gemmlowp headers.

The exact upstream license texts are in `licenses/`. The model data and the
DVP/V4L2 service were created for this project and are not copied from an
upstream TensorFlow example.

The program dynamically uses the SDK's uClibc runtime. Its LGPL and other
file-specific notices are covered by the SDK legal materials.
