#Things To Do in the Future

- power4ctl and power4d do their architecture builds differently,
  maybe homogenize.
- Maybe a makefile run target for each program so I don't have to hunt
  down their executables?
- The house example exposes a fatal problem if policy can't run.  We
  need some sort of fallback, ideally respecting switches.
- power4d should be able to listen to the ethernet gateway port and
  stash the files somewhere in /run
- power4d should be able to listen to the ethernet gateway port and
  print the received files
