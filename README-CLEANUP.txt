KOYODA Gyro Asset Cleanup
=========================

PURPOSE
-------
Clean the old six-face Gyro asset references while preserving the current
stable Wi-Fi Status firmware.

KEEP
----
main/fun_happy.c
  Used by Tap Wake:
  Closed -> Half -> Idle -> Happy -> Idle

main/fun_peak_thrill.c
  Reserved for the future one-frame Gyro reaction.
  It remains in GitHub but is intentionally NOT listed in main/CMakeLists.txt,
  so it does NOT consume ESP32 flash yet.

REPLACE THESE FILES
-------------------
1) main/CMakeLists.txt
2) main/fun_faces.h
3) components/koyoda_motion_probe/fun_faces.h

FILES THAT SHOULD ALREADY BE DELETED
------------------------------------
main/fun_excited.c
main/fun_tilt.c
main/fun_drop.c
main/fun_dizzy.c

IMPORTANT ABOUT BOARD STORAGE
-----------------------------
Only image .c files listed in main/CMakeLists.txt are linked into the firmware.
With this cleanup:
- fun_happy.c = compiled, because Tap Wake uses it.
- fun_peak_thrill.c = stored in GitHub only; zero firmware cost for now.

OPTIONAL REPOSITORY CLEANUP
---------------------------
The following are old, disabled Gyro experiments and are NOT currently
compiled by main/CMakeLists.txt:

main/koyoda_motion_inline.c
main/koyoda_motion_inline.h

components/koyoda_motion_probe/koyoda_motion_ui.c
components/koyoda_motion_probe/koyoda_motion_ui.h
components/koyoda_motion_probe/fun_happy.c
components/koyoda_motion_probe/fun_peak_thrill.c

You may delete those six files later to make the GitHub repository smaller.
They do not currently consume ESP32 flash, so deleting them is optional and
is NOT required for this cleanup to build.

DO NOT DELETE
-------------
main/fun_happy.c
main/fun_peak_thrill.c
main/koyoda_animation.h
main/koyoda_wifi.c
main/koyoda_wifi.h
main/main.c

No Gyro sensor code is enabled by this package.
