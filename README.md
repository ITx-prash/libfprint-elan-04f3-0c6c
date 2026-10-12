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

Because `libfprint` drivers are compiled into the library itself, you need to build from source. We use Davide Depau's `elanmoc2` fork as the base since it already contains the required driver structure, and replace the driver files with this patched version.

> [!IMPORTANT]
> **Arch Linux and Omarchy users:** skip the steps below and see [Arch Linux / Omarchy](#arch-linux--omarchy) instead. These generic steps install to `/usr/local`, which Arch's dynamic linker does not search, so they appear to succeed and then fail with `no such device`.

### 1. Install Dependencies

**Ubuntu / Debian**

```bash
sudo apt update
sudo apt install build-essential meson ninja-build git pkg-config \
  libglib2.0-dev libgusb-dev libssl-dev libpixman-1-dev \
  libgudev-1.0-dev libudev-dev gtk-doc-tools libgirepository1.0-dev \
  python3-cairo python3-gi fprintd libpam-fprintd
```

**Fedora**

```bash
sudo dnf install fprintd fprintd-pam git meson ninja-build gcc gcc-c++ \
  glib2-devel libgusb-devel pixman-devel libusb1-devel \
  gobject-introspection-devel openssl-devel libgudev-devel gtk-doc
```

### 2. Clone Davide Depau's elanmoc2 fork

```bash
git clone -b elanmoc2 https://gitlab.freedesktop.org/Depau/libfprint.git
```

### 3. Clone This Repository

```bash
git clone https://github.com/ITx-prash/libfprint-elan-04f3-0c6c.git
```

### 4. Replace the Driver Files

```bash
cp libfprint-elan-04f3-0c6c/src/elanmoc2.{c,h} libfprint/libfprint/drivers/elanmoc2/
```

### 5. Build and Install

**Ubuntu / Debian**

```bash
cd libfprint
meson setup builddir
ninja -C builddir
sudo ninja -C builddir install
sudo ldconfig
```

**Fedora**

```bash
cd libfprint
meson setup builddir
ninja -C builddir
sudo ninja -C builddir install

# Required: Fedora uses lib64 paths — this ensures fprintd loads the
# patched library instead of the stock one
echo "/usr/local/lib64" | sudo tee /etc/ld.so.conf.d/libfprint-elan.conf
sudo ldconfig
```

### 6. Start the Fingerprint Service

**Ubuntu / Debian**

```bash
sudo systemctl restart fprintd
```

**Fedora**

```bash
sudo systemctl restart fprintd

# Enable fingerprint authentication for sudo and login
sudo authselect enable-feature with-fingerprint
sudo authselect apply-changes
```

> [!WARNING]
> **System Updates:** Package managers may reinstall the stock `libfprint` during system updates and override this patched library. If your fingerprint suddenly stops working after an update, simply re-run the steps from Step 5 onwards. On Fedora, updates may also reset your `ldconfig` configuration. (The Arch package in [`packaging/arch/`](packaging/arch/PKGBUILD) is not affected: pacman tracks it and will not silently replace it.)

## Arch Linux / Omarchy

On Arch the generic steps above appear to succeed and then do nothing. `meson setup builddir` defaults to `prefix=/usr/local`, but `/usr/local/lib` is not on Arch's dynamic linker search path. `/etc/ld.so.conf.d/` carries no entry for it on a stock install, and glibc's built-in trusted directories are only `/lib` and `/usr/lib`. So `sudo ldconfig` is a no-op, `fprintd` goes on loading the stock `/usr/lib/libfprint-2.so.2`, and enrollment fails with `Impossible to enroll: no such device`.

You can confirm that is what happened:

```bash
ldd /usr/lib/fprintd | grep fprint                      # => /usr/lib/libfprint-2.so.2
strings /usr/lib/libfprint-2.so.2 | grep -c elanmoc2    # => 0
```

(Debian and Ubuntu list `/usr/local/lib` in `ld.so.conf` by default, which is why the generic steps work there.)

Build a package instead, so pacman owns the library and installs it to `/usr/lib`:

```bash
sudo pacman -S --needed base-devel git fprintd
git clone https://github.com/ITx-prash/libfprint-elan-04f3-0c6c.git
cd libfprint-elan-04f3-0c6c/packaging/arch
makepkg -si
sudo systemctl restart fprintd
```

[`packaging/arch/PKGBUILD`](packaging/arch/PKGBUILD) clones Depau's fork and this repository itself, so steps 2–4 above are not needed. Pacman will ask to remove the existing `libfprint` (or `libfprint-git`). Accept it. The replacement satisfies `fprintd`'s `libfprint-2.so=2-64` dependency in the same transaction, so the daemon is never left without a library.

Verify with `fprintd-list $USER`, which should report `found 1 devices`.

Because the package is foreign to pacman (it is in no repository), neither `pacman -Syu` nor `omarchy update` will replace it.

### Omarchy

Omarchy ships a wizard that wires up PAM for `sudo`, polkit and the lock screen, including a clamshell gate so that a closed lid falls straight through to the password prompt instead of blocking on an unreachable sensor. Prefer it over editing PAM by hand:

```bash
omarchy setup security fingerprint
```

Run it **after** installing the package. On a stock Omarchy system it detects this reader (it matches on the `ELAN:ARM-M4` product string), installs `libfprint-git` from the `[omarchy]` repository, then fails at enrollment, because that build has no driver for `04f3:0c6c`, and exits before configuring PAM.

> [!IMPORTANT]
> The wizard guards its package install with `pacman -Q libfprint-git`, which resolves `provides`. The PKGBUILD therefore declares `provides=('libfprint-git')` so that check passes and the wizard leaves your patched library in place. Without it the wizard runs `pacman -S --needed --noconfirm --ask 4 libfprint-git`, whose `--ask 4` silently accepts replacing the patched package with the stock one. Fingerprint then stops working again, with nothing on screen to say why.

> [!NOTE]
> Depau's fork is based on libfprint **1.94.9**, older than what Arch and Omarchy currently ship. Installing this trades newer fixes for *other* drivers against support for this sensor.

## Verify It Works

```bash
fprintd-list $USER
```

If you see `found 1 devices`, you're good to go.

## Enroll & Use

```bash
fprintd-enroll -f right-index-finger $USER
```

Tap your finger on the sensor repeatedly when prompted. Once complete, verify it works:

```bash
fprintd-verify $USER
```

When it says `verify-match (done)`, your fingerprint is ready to use.

Once the device is detected, fingerprint enrollment is usually available directly from your desktop environment's system settings. For **GNOME**, **KDE Plasma**, and **Cinnamon** users, this typically requires no additional configuration. If you are using a minimal or tiling window manager, you may need to configure PAM manually.

## Troubleshooting

- **No device found after install (Fedora):** Ensure the `lib64` ldconfig entry exists — re-run the `echo` and `ldconfig` commands from Step 5, then restart `fprintd`.
- **No device found after install (Ubuntu/Debian):** Run `sudo ldconfig` then `sudo systemctl restart fprintd`. If still not found, suspend and resume your laptop or reboot to reset the sensor's USB state.
- **No device found after install (Arch/Omarchy):** `sudo ldconfig` will not help, because `/usr/local/lib` is not on Arch's linker path. Use [Arch Linux / Omarchy](#arch-linux--omarchy).
- **Fingerprint not working for sudo or login (Ubuntu/Debian):** Ensure `libpam-fprintd` is installed — `sudo apt install libpam-fprintd` — then re-enroll.
- **Fingerprint not working for sudo or login (Fedora):** Run `sudo authselect enable-feature with-fingerprint && sudo authselect apply-changes` then re-enroll.
- **Errors during enroll/verify:** Run `fprintd-delete $USER`, restart the service with `sudo systemctl restart fprintd`, and try again.

## Uninstallation

From the same `libfprint` directory you built in:

```bash
sudo ninja -C builddir uninstall
sudo ldconfig
```

On Arch/Omarchy, remove the package and restore the stock library instead:

```bash
sudo pacman -Rns libfprint-elanmoc2-0c6c
sudo pacman -S libfprint          # or libfprint-git on Omarchy
sudo systemctl restart fprintd
```

## Credits

- Original `elanmoc2` driver by **Davide Depau** for the ELAN `04f3:0c4c` sensor — [Original Work](https://gitlab.freedesktop.org/Depau/libfprint/-/tree/elanmoc2) | [Original MR](https://gitlab.freedesktop.org/libfprint/libfprint/-/merge_requests/330)
- Adapted and fixed for the `04f3:0c6c` sensor by **Prashant Adhikari**

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
