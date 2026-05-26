# clevo-indicator

Dual-fan control tray app for Clevo barebone laptops on Linux. Shows CPU/GPU temperature and fan duty, with auto control, linked manual speeds, or independent CPU/GPU fan control.

Forked and extended from:

- [SkyLandTW/clevo-indicator](https://github.com/SkyLandTW/clevo-indicator) 
- [SkyLandTW/ClevoECView](https://github.com/SkyLandTW/ClevoECView) 

## Hardware

- **Tested on: Clevo P770ZM** (and likely other P770 / similar Clevo barebones)
- Clevo barebone laptop (e.g. Hasee, Mechrevo, and similar OEMs)
- Clevo-style EC register layout (other brands are unsupported)
- Dual-fan chassis (CPU + GPU fans)

## System

- Linux desktop (tested on **Ubuntu 26.04 / GNOME**)
- System tray with **Ubuntu AppIndicators** extension enabled

## Install

```bash
sudo apt update
sudo apt install -y build-essential pkg-config \
  libayatana-appindicator3-dev libgtk-3-dev \
  gnome-shell-extension-appindicator

make clean && make && sudo make install
```

This installs `clevo-indicator` to **`/usr/local/bin/`** with setuid root (`root:adm`, mode `4750`).

If the tray icon does not appear, enable **Ubuntu AppIndicators** under Extensions, then log out and back in.

## Usage

Run from a graphical session as a normal user (not headless SSH without `DISPLAY`):

```bash
clevo-indicator          # tray mode
clevo-indicator 80       # both fans at 80%
clevo-indicator 80 60    # CPU 80%, GPU 60%
clevo-indicator -?       # help and current EC status
```

Tray label `C65 G58 F80 F60` = CPU °C, GPU °C, CPU fan %, GPU fan %. Click the icon for AUTO, linked 60–100%, or per-fan CPU/GPU presets.

The app forks into a UI process (desktop user) and an EC worker (root) so the tray can show while EC I/O stays privileged; you may see two processes in `ps`.

## Notes

- After install, verify setuid: `ls -l /usr/local/bin/clevo-indicator` must show **`rws`** (not only `rwx`). If missing, run `sudo chmod u+s /usr/local/bin/clevo-indicator`
- Your user must be in the **`adm`** group to run the installed binary (`sudo usermod -aG adm $USER`, then log out and back in)
- If EC reads fail, try `sudo modprobe ec_sys`
- Fan changes affect hardware — test with moderate manual speeds first
- If GPU fan readings stay at 0, your model may use different EC registers

## Disclaimer

This project was modified and tested **only on my own machine** (Clevo P770ZM). If you use it on other hardware, you do so **at your own risk**. I am **not responsible** for any damage, data loss, overheating, or other issues that may result from using this software.

License: see [LICENSE](LICENSE) (Unlicense).
