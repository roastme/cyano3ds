# Dumping the AR6014G Wi-Fi firmware with GodMode9

The Linux/Android port has **no driver for the 3DS internal NAND**, so Android
cannot read the Wi-Fi firmware itself — it lives inside the console's **NWM**
system module (`00040130/00002d02`) on SysNAND.  The firmware has to be pulled
off the console with GodMode9, and that is what these scripts do.

For the full background see [`docs/WIFI.md`](../../docs/WIFI.md).

## What is here

| File | Purpose |
|---|---|
| `DumpNWMWifi.lua` | Modern GodMode9 (v2.2.0+) **Lua** script.  **Use this one.** |
| `DumpNWMWifi.gm9` | Legacy GM9Script fallback for GodMode9 < v2.2.0. |

Both only **read** the NAND and write to `0:/gm9/out/wifi/` on the SD card.
Nothing on the console is modified.

## Running it

1. Copy `DumpNWMWifi.lua` to `sd:/gm9/luascripts/` **or** anywhere else on the card.
2. Boot GodMode9.
3. Either:
   * press **HOME → `Lua scripts...` → DumpNWMWifi**, or
   * navigate to `DumpNWMWifi.lua` and choose **`Execute Lua script`**.
4. Wait for it to finish (a few seconds).  It prints what it found.

## Output

```
0:/gm9/out/wifi/
├── nwm_00002d02_00000000.app    raw NCCH backup (still encrypted)
├── nwm_00002d02_00000000.code   decompressed ExeFS .code   <-- the firmware source
├── ...                          (safe-mode NWM if present)
└── README.txt                   what was found
```

`title.extract_code` does the decryption and the BLZ decompression for us, so
the `.code` is directly usable by the PC-side extractor.

## Then, on the PC

Carve the six Xtensa blocks out of the `.code` (this is the GBATEK
literal-pool recipe):

```bash
python3 port/scripts/nwm-extract.py /path/to/nwm_00002d02_00000000.code
```

It writes `nwm_...code.blocks/` containing `stub_data.bin`, `stub_code.bin`,
`database.bin`, `main_type1.bin`, `main_type4.bin`, `main_type5.bin` and a
`blocks.json` manifest (addresses, offsets, sizes, SHA-256).  The invariable
block sizes (Stub.data `0x38`, Stub.code `0x316`, Database `0x1E8`) are used to
confirm the code base was found correctly.

Copy the carved `stub_data.bin`, `stub_code.bin`, `main_type4.bin` and
`database.bin` into `firmware/ath6k/AR6002/nwm/` in the checkout (or set
`NWM_DIR` to the directory holding them) so `mkinitramfs.sh` picks them up.
See [docs/BUILD.md](../../docs/BUILD.md#7-firmware-optional-wi-fi-only).
