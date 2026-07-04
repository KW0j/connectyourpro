<div align="center">

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/logo-dark.svg">
  <img src="docs/logo-light.svg" alt="connectyourpro" width="440">
</picture>

**All-in-one Nintendo Switch 1/2 Pro Controller on PC solution.**

[Download](https://github.com/KW0j/connectyourpro/releases/latest) · [Website](https://connectyour.pro) · [FAQ](#faq)

</div>

---

The Switch 2 Pro Controller does nothing when you plug it into a PC — it waits for a
proprietary activation sequence that only the console normally sends. **connectyourpro
performs that activation automatically** and turns the controller into a standard
Xbox 360 (XInput) or DualShock 4 gamepad that every PC game understands.

## Features

- **Switch 2 Pro Controller** over **USB** (recommended) and **Bluetooth** (experimental)
- **Switch 1 Pro Controller** over USB and Bluetooth
- Built-in activation — the [procon2tool](https://handheldlegend.github.io/procon2tool/)
  "Enable HID Output" step happens automatically inside the app
- Shows up in games as **Xbox 360 (XInput)** or **DualShock 4** — switchable in the app
- **Live controller test view** — pressed buttons light up on a schematic, sticks move in real time
- **Button remapping** — click a button on the schematic and pick what it outputs
- **DSU server** (Cemuhook protocol) — gyro/motion for Dolphin, Cemu and other emulators
- Settings persist between sessions (`connectyourpro.ini`)

## Installation

1. Grab **connectyourpro-setup** from the
   [latest release](https://github.com/KW0j/connectyourpro/releases/latest).
2. Run it — the installer also sets up the [ViGEmBus](https://github.com/nefarius/ViGEmBus)
   driver (required for virtual gamepads) if it's not present.
3. Launch connectyourpro, pick your controller, connection and output type, hit **Connect**.

### Bluetooth notes

- **Pro Controller 2**: do **not** pair it in Windows settings. Pick Bluetooth in the app,
  press Connect, then hold the controller's SYNC button until the LEDs sweep — the app
  finds it by itself. Bluetooth on the ProCon 2 is experimental and has noticeably more
  input lag than USB.
- **Pro Controller 1**: pair it in Windows Bluetooth settings first, then press Connect.

## Building from source

Requirements: Visual Studio 2022 (Desktop C++ workload, Windows SDK), CMake.

```bat
build_release.bat
```

The executable lands in `build\testapp.exe`. The installer script lives in
`installer\connectyourpro.iss` (Inno Setup 6; drop the ViGEmBus setup exe into
`installer\redist\ViGEmBus_Setup.exe` before compiling).

## FAQ

**The "Pro Controller" entry in Windows game controllers shows random button clicks.**
That's the *physical* controller — after activation it speaks a protocol Windows doesn't
understand, so the built-in test panel shows garbage. Games use the virtual
**Xbox 360 Controller** instead. Ignore the Pro Controller entry.

**The controller stopped connecting at all.**
These controllers have a built-in cooldown: too many connect attempts in a short time and
they sulk for a few minutes. Wait a bit and try again.

**No input after Connect (USB).**
Unplug the cable, plug it back in, press Connect again. As a last resort the app has a
fallback button that opens procon2tool in the browser.

**Does it work with Steam?**
Yes — Steam sees the virtual Xbox 360 / DS4 pad like any other controller.

## Credits

- Based on [joycon2cpp](https://github.com/TheFrano/joycon2cpp) by **Frano (TheFrano)** —
  the original Switch 2 BLE research and implementation this project grew from. many thanks, amazing work!!
- [procon2tool](https://handheldlegend.github.io/procon2tool/) by **HandheldLegend** —
  the USB activation sequence.
- **[@german77](https://github.com/german77)** — Joy-Con 2 notification layout research.
- [ViGEmBus](https://github.com/nefarius/ViGEmBus) by **Nefarius Software Solutions** —
  virtual gamepad driver.
- [Dear ImGui](https://github.com/ocornut/imgui) — UI.
- dekuNukem's [Nintendo Switch Reverse Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering)
  docs — Switch 1 Pro Controller protocol.

## License

[MIT](LICENSE). This project is not affiliated with, endorsed by, or sponsored by
Nintendo. "Nintendo Switch" and "Pro Controller" are trademarks of Nintendo.
