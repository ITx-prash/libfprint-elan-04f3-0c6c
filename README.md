<div align="center">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="assets/fingerprint-dark.svg">
      <source media="(prefers-color-scheme: light)" srcset="assets/fingerprint.svg">
      <img alt="Fingerprint Logo" src="assets/fingerprint.svg" width="120">
    </picture>
    <h1>libfprint-elan-04f3-0c6c</h1>
    <img src="https://img.shields.io/badge/License-LGPL_v2.1-blue.svg" alt="License: LGPL v2.1">
    <img src="https://img.shields.io/badge/Platform-Linux-lightgrey.svg" alt="Platform: Linux">
</div>

---

A working Linux driver for the **ELAN 04f3:0c6c** fingerprint sensor (ELAN Match-on-Chip 2), currently [unsupported by the official libfprint project](https://gitlab.freedesktop.org/libfprint/libfprint/-/work_items/423).

Based on Davide Depau's `elanmoc2` driver for the `04f3:0c4c` sensor, adapted for `04f3:0c6c` via Wireshark USB traffic analysis of the official Windows driver. Key differences resolved: **0-indexed hardware slot addressing** and an ARM-M4 specific pre-commit slot marker sequence (`00 10 <slot>`) observed in the collision-check path before commit.

> [!NOTE]
> This driver modifies a core system library. While it has been tested and is working, use it at your own risk. Always keep your password as a fallback authentication method.

## Verify Your Device

Before proceeding, confirm this driver is for your device:

```bash
lsusb | grep 04f3
```

You should see an entry containing `04f3:0c6c`. If not, this driver is not for your device.

## Installation

Because `libfprint` drivers are compiled directly into the library itself rather than loaded as plugins, the patched library must be built from source. We use Davide Depau's `elanmoc2` fork as the base and replace the driver files with this `04f3:0c6c` patch.

Select the instructions for your distribution:

### Ubuntu / Debian

1. **Install dependencies:**

   ```bash
   sudo apt update
   sudo apt install build-essential meson ninja-build git pkg-config \
     libglib2.0-dev libgusb-dev libssl-dev libpixman-1-dev \
     libgudev-1.0-dev libudev-dev gtk-doc-tools libgirepository1.0-dev \
     python3-cairo python3-gi fprintd libpam-fprintd
   ```

2. **Clone and patch the driver:**

   ```bash
   git clone -b elanmoc2 https://gitlab.freedesktop.org/Depau/libfprint.git
   git clone https://github.com/ITx-prash/libfprint-elan-04f3-0c6c.git
   cp libfprint-elan-04f3-0c6c/src/elanmoc2.{c,h} libfprint/libfprint/drivers/elanmoc2/
   ```

3. **Build and install:**
   ```bash
   cd libfprint
   meson setup builddir
   ninja -C builddir
   sudo ninja -C builddir install
   sudo ldconfig
   sudo systemctl restart fprintd
   ```

---

### Fedora

1. **Install dependencies:**

   ```bash
   sudo dnf install fprintd fprintd-pam git meson ninja-build gcc gcc-c++ \
     glib2-devel libgusb-devel pixman-devel libusb1-devel \
     gobject-introspection-devel openssl-devel libgudev-devel gtk-doc
   ```

2. **Clone and patch the driver:**

   ```bash
   git clone -b elanmoc2 https://gitlab.freedesktop.org/Depau/libfprint.git
   git clone https://github.com/ITx-prash/libfprint-elan-04f3-0c6c.git
   cp libfprint-elan-04f3-0c6c/src/elanmoc2.{c,h} libfprint/libfprint/drivers/elanmoc2/
   ```

3. **Build, install, and configure:**

   ```bash
   cd libfprint
   meson setup builddir
   ninja -C builddir
   sudo ninja -C builddir install

   # Fedora uses lib64 for dynamic libraries
   echo "/usr/local/lib64" | sudo tee /etc/ld.so.conf.d/libfprint-elan.conf
   sudo ldconfig

   sudo systemctl restart fprintd
   sudo authselect enable-feature with-fingerprint
   sudo authselect apply-changes
   ```

---

### Arch Linux / Omarchy

1. **Install dependencies and build the package:**

   ```bash
   sudo pacman -S --needed base-devel git fprintd
   git clone https://github.com/ITx-prash/libfprint-elan-04f3-0c6c.git
   cd libfprint-elan-04f3-0c6c/packaging/arch
   makepkg -si
   sudo systemctl restart fprintd
   ```

2. **Configure authentication:**
   - **Omarchy:** Run the built-in fingerprint wizard to configure PAM for sudo, polkit, and the lock screen:
     ```bash
     omarchy setup security fingerprint
     ```
   - **Arch Linux:** Proceed directly to [Enroll & Use](#enroll--use).

---

> [!WARNING]
> **System Updates:** On Debian, Ubuntu, and Fedora, package manager updates may reinstall the stock `libfprint` and overwrite this patched version. If your fingerprint stops working after an update, simply re-run the build and install steps. (On Arch Linux and Omarchy, pacman tracks the package and will not overwrite it).

## Verify It Works

```bash
fprintd-list $USER
```

If you see `found 1 devices`, your sensor is recognized.

## Enroll & Use

```bash
fprintd-enroll -f right-index-finger $USER
```

Tap your finger on the sensor repeatedly when prompted until enrollment completes. Then verify:

```bash
fprintd-verify $USER
```

When it outputs `verify-match (done)`, fingerprint authentication is ready.

On desktop environments such as **GNOME**, **KDE Plasma**, and **Cinnamon**, fingerprint enrollment is also available directly in System Settings. If you use a minimal or tiling window manager, you may need to configure PAM manually.

## Troubleshooting

- **No device found after install (Fedora):** Ensure the `lib64` ldconfig entry exists — re-run the `echo` and `ldconfig` commands from the Fedora install section, then restart `fprintd`.
- **No device found after install (Ubuntu / Debian):** Run `sudo ldconfig` then `sudo systemctl restart fprintd`. If still not found, suspend and resume your laptop or reboot to reset the sensor's USB state.
- **Fingerprint not working for sudo or login (Ubuntu / Debian):** Ensure `libpam-fprintd` is installed (`sudo apt install libpam-fprintd`) and PAM authentication is enabled (`sudo pam-auth-update`).
- **Fingerprint not working for sudo or login (Fedora):** Ensure PAM is enabled: `sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes`.
- **Errors during enroll/verify:** Clear enrolled fingers with `fprintd-delete $USER`, restart the daemon with `sudo systemctl restart fprintd`, and try again.

## Uninstallation

**Ubuntu / Debian / Fedora**

From the `libfprint` directory you built in:

```bash
sudo ninja -C builddir uninstall
sudo ldconfig
```

_(On Fedora, also remove `/etc/ld.so.conf.d/libfprint-elan.conf`)_

**Arch Linux / Omarchy**

Remove the package and restore the standard library:

```bash
sudo pacman -Rns libfprint-elanmoc2-0c6c
sudo pacman -S libfprint          # or libfprint-git on Omarchy
sudo systemctl restart fprintd
```

## Credits

- Original `elanmoc2` driver by **Davide Depau** for the ELAN `04f3:0c4c` sensor — [Original Work](https://gitlab.freedesktop.org/Depau/libfprint/-/tree/elanmoc2) | [Original MR](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/330)
- Adapted and fixed for the `04f3:0c6c` sensor by **Prashant Adhikari**

Thanks to all [contributors](https://github.com/ITx-prash/libfprint-elan-04f3-0c6c/graphs/contributors) who help improve packaging, documentation, and support!

---

<p align="center">
    <img src="assets/coder.png" height="150" alt="Coder illustration"/>
    <br/>
    <em>Crafted with 💚 on GNU/Linux</em>
    <br/>
    Copyright &copy; 2026-present <a href="https://github.com/ITx-prash" target="_blank">Prashant Adhikari</a>
    <br/><br/>
    <a href="LICENSE"><img src="https://img.shields.io/static/v1.svg?style=for-the-badge&label=License&message=LGPL-2.1&logoColor=a6e3a1&colorA=1e1e2e&colorB=a6e3a1"/></a>
</p>
