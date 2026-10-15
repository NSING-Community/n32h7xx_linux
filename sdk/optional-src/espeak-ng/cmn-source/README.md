# Reconstructing the published Mandarin dictionary

These editable inputs reproduce the existing 998551-byte `cmn_dict`. They
derive from espeak-ng commit ba90c8e9f440ad544f674a790bb5f53878b6ffc5.
Only `cmn_listx` is a subset: 59064 of 77860 compiled entries are retained.
Original comments, directives and attribution remain. This restores an
equivalent preferred source, not a claim about the original selection method.
`recovery.json` records the expected dictionary SHA-256.

Using a native espeak-ng built from that source and the supplied port, with
the matching generated phoneme/intonation data in a separate scratch directory:

```sh
mkdir -p "$WORK/dictsource"
cp cmn_list cmn_listx cmn_emoji cmn_rules "$WORK/dictsource/"
cp -a "$MATCHING_DATA" "$WORK/espeak-ng-data"
(cd "$WORK/dictsource" && ESPEAK_DATA_PATH="$WORK" "$NATIVE_ESPEAK" --compile=cmn)
sha256sum "$WORK/espeak-ng-data/cmn_dict"
```

Set WORK, MATCHING_DATA and NATIVE_ESPEAK to absolute paths. This operation
only writes to WORK. See the port Makefile for native/data build targets.
The other shipped data files are the preserved upstream data build output.

The espeak-ng source license and original Unicode/CC-CEDICT input attribution
apply; see the optional drop's licenses/Mandarin-NOTICE.txt. Do not relabel
these third-party data inputs as project-owned BSD material.
