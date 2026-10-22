# input-events

Minimal Linux evdev reader for the HMI touchscreen. It has no libevdev or
Android dependency.

```sh
make input-events
out/input-events/input-events /dev/input/event0
```

Touch the panel and look for `ABS_MT_POSITION_X`, `ABS_MT_POSITION_Y`,
`ABS_MT_TRACKING_ID`, `BTN_TOUCH`, and `EV_SYN`. Press Ctrl-C to stop.
