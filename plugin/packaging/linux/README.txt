OBS Tab Player - install on Linux
=================================

For OBS Studio 30 or newer installed from the OBS PPA (Ubuntu), your distribution or
OBS's .deb. Built on Ubuntu 24.04 (x86_64).

1. Close OBS.
2. In this folder, run:  ./install.sh
   It copies the plugin into ~/.config/obs-studio/plugins (no root needed).
3. Start OBS. The "Tab Player" panel shows two buttons:
     1. Choose your tabs folder
     2. Add Tab Player to this scene
   Pick a tab, press Play. Demo tabs are in the demo-tabs folder here.

The Audio Mixer gets two faders: "Tab Player" (the guitar) and
"Tab Player Metronome" (the clicks).

Prefer a system package? Install obs-tab-player_<version>_amd64.deb instead.
The Flatpak version of OBS needs a Flatpak plugin; this package does not load there.

Uninstall: ./uninstall.sh
https://bask209.github.io/obs-tab-player/
