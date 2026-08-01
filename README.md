# Cold Brew

Cold Brew is an Aroma plugin for Wii U that hides selected accounts from the
user selector shown during boot. It changes only what the Wii U Menu displays;
it never deletes an account or modifies account data.

## Features

- Configure each Wii U account by Mii name, NNID, and physical slot number.
- Mark accounts as **Visible** or **Hidden** from the Aroma plugin menu.
- Save the visibility mask in Aroma plugin storage on the SD card.
- Keep at least one existing account visible at all times.
- Apply a changed setting by restarting the Wii U Menu once when the plugin
  menu closes.
- Intercept ACT calls only in the Wii U Menu process, not in games or other
  applications.

## Installation and configuration

1. Copy `cold_brew.wps` to
   `sd:/wiiu/environments/aroma/plugins/`.
2. Boot the Wii U through Aroma.
3. Press `L + D-pad Down + MINUS (-)` to open the Aroma plugin menu.
4. Open **Cold Brew** and choose the accounts that should be **Hidden**.
5. Close the plugin menu. When a value changed, Cold Brew saves it and restarts
   the Wii U Menu automatically. The updated selector appears after the
   restart.

Cold Brew refuses to hide the last visible account. If you try, that account
remains **Visible**.

> When Wii U automatic login is enabled, the user selector itself may be
> skipped. Cold Brew does not change the automatic-login setting.

## Build

With Docker:

```sh
docker build -t cold-brew-builder .
docker run --rm -v "${PWD}:/project" cold-brew-builder make
```

Or use a devkitPro environment with devkitPPC, wut, and WiiUPluginSystem:

```sh
make
```

The output file is `cold_brew.wps`.

When using the native devkitPro Make rules, build from a path without spaces.

## How it works

The Wii U Menu numbers user accounts by physical slots from 1 through 12.
Cold Brew exposes only the visible accounts as compact virtual slots `1..N`.
For example, if physical slot 2 is the only visible account, the selector sees
it as virtual slot 1; selecting it is translated back to physical slot 2.

After a selection, Cold Brew also keeps the active account slot returned to the
Wii U Menu consistent with that virtual slot. Mapping and re-entrancy state are
kept per Wii U Menu thread so simultaneous account queries cannot bypass the
filter.

Cold Brew does not call account creation, deletion, movement, or NAND-save
APIs.

## Compatibility

Cold Brew supports Wii U user slots 1 through 12 and targets the Wii U Menu
process only.

## License

[MIT](LICENSE)
