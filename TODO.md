#Things To Do in the Future

- power4ctl and power4d do their architecture builds differently,
  maybe homogenize.
- Maybe a makefile run target for each program so I don't have to hunt
  down their executables?
- The house example exposes a fatal problem if policy can't run.  We
  need some sort of fallback, ideally respecting switches.
- The batteries can get stuck at the bluetooth level and need a reboot
  to clear.
- power4ctl should do DNS lookup on address and validate the IP, right
  now it fails at the first command issued in interative mode.
  
