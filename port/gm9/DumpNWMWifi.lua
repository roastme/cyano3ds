--[[
    DumpNWMWifi.lua -- dump the Nintendo 3DS "NWM" module so the AR6014G
    Xtensa Wi-Fi firmware can be extracted for the ath6kl Linux driver.

    Background
    ----------
    The 3DS Wi-Fi chip (Atheros AR6014G, SDIO) runs Xtensa firmware that the
    console uploads from its NAND at every boot.  That firmware lives inside
    the system module "NWM" (title 00040130 / 00002d02), more precisely inside
    the module's ExeFS ".code", as several Xtensa blobs (Stub/Database plus the
    Main Type1/Type4/Type5 images -- see GBATEK "3DS Files - Module NWM").

    There is no NAND driver in the Linux/Android port, so Android cannot read
    the firmware itself.  This script runs in GodMode9 instead: it finds the
    NWM title(s) on SysNAND, copies the raw NCCH, and extracts the decompressed
    ExeFS ".code" to the SD card.  A companion PC tool (nwm-extract.py) then
    carves out the Xtensa images and turns them into ath6kl firmware files.

    Output (on the SD card)
    -----------------------
      0:/gm9/out/wifi/nwm_<tid>_<index>.app   raw NCCH backup (still encrypted)
      0:/gm9/out/wifi/nwm_<tid>_<index>.code  decompressed ExeFS .code  <-- the good stuff
      0:/gm9/out/wifi/README.txt              what was found

    Run it
    ------
      * HOME button -> "Lua scripts..." -> DumpNWMWifi   (place in 0:/gm9/luascripts)
      * or navigate to DumpNWMWifi.lua -> "Execute Lua script"

    Only reads NAND and writes to 0:/gm9/out -- nothing on the console changes.
]]

local OUTDIR = "0:/gm9/out/wifi"

-- Normal mode NWM is present on every 3DS.  The two safe-mode titles exist on
-- some models and hold the same kind of Xtensa blocks, so we grab them too if
-- they are there.
local CANDIDATES = {
    { tid = "00040130/00002d02", tag = "nwm"       }, -- NWM normal mode (all)
    { tid = "00040130/00002d03", tag = "nwm_safe"  }, -- NWM safe mode (Old3DS)
    { tid = "00040130/20002d03", tag = "nwm_safe_n3ds" }, -- NWM safe (New3DS)
}

local report = {}

local function note(s)
    print(s)
    report[#report + 1] = s
end

local function ends_with_app(name)
    return string.lower(string.sub(name, -4)) == ".app"
end

-- Dump one title.  root is "1:" (SysNAND CTRNAND).  Returns the number of
-- .code files written.
local function dump_title(root, tid, tag)
    local dir = root .. "/title/" .. tid
    if not fs.exists(dir) then
        note("absent : " .. tid .. " (" .. tag .. ")")
        return 0
    end
    note("found  : " .. tid .. " (" .. tag .. ")")

    local contentdir = dir .. "/content"
    if not fs.is_dir(contentdir) then
        note("  no content dir: " .. contentdir)
        return 0
    end

    local entries
    local ok, err = pcall(fs.list_dir, contentdir)
    if not ok then
        note("  list_dir failed: " .. tostring(err))
        return 0
    end
    entries = err

    local written = 0
    for _, e in ipairs(entries) do
        if e.type == "file" and ends_with_app(e.name) then
            local app  = contentdir .. "/" .. e.name
            local stem = OUTDIR .. "/" .. tag .. "_" .. string.sub(e.name, 1, -5)
            local appcopy = stem .. ".app"
            local code    = stem .. ".code"

            note("  app    : " .. e.name .. " (" .. tostring(e.size) .. " bytes)")

            -- 1. raw backup (encrypted NCCH), useful if the extraction below
            --    ever needs to be redone on the PC.
            local okc, errc = pcall(fs.copy, app, appcopy,
                                    { no_cancel = true, overwrite = true })
            if okc then
                note("  copied : " .. appcopy)
            else
                note("  copy failed: " .. tostring(errc))
            end

            -- 2. Extract the ExeFS ".code" (GodMode9 decrypts + decompresses).
            --    Try the NAND file directly first, then the SD copy.
            local oke, erre = pcall(title.extract_code, app, code)
            if not oke then
                note("  extract from NAND failed: " .. tostring(erre))
                if okc then
                    oke, erre = pcall(title.extract_code, appcopy, code)
                    if not oke then
                        note("  extract from SD copy failed: " .. tostring(erre))
                    end
                end
            end
            if oke then
                local st = fs.exists(code) and fs.stat(code) or nil
                note("  code   : " .. code ..
                     (st and (" (" .. tostring(st.size) .. " bytes)") or ""))
                written = written + 1
            end
        end
    end
    return written
end

-- --- main ----------------------------------------------------------------

ui.echo("DumpNWMWifi\nGodMode9 " .. tostring(GM9VER) ..
        "\n \nDumping the NWM (Wi-Fi firmware)\nmodule to " .. OUTDIR ..
        "\n \nPress (A) to start.")

pcall(fs.mkdir, OUTDIR)

note("DumpNWMWifi -- " .. tostring(GM9VER) ..
     " -- " .. tostring(os.date("%Y-%m-%d %H:%M:%S")))
note("output : " .. OUTDIR)

-- The NAND has to be mounted; "1:" is SysNAND CTRNAND in GodMode9.
if not fs.exists("1:/title") then
    note("ERROR: 1:/title not visible -- is the NAND mounted?")
else
    local total = 0
    for _, c in ipairs(CANDIDATES) do
        total = total + dump_title("1:", c.tid, c.tag)
    end

    -- If the known TIDs were not enough, scan 00040130 for any other 00002dxx.
    if total == 0 then
        note("scanning 1:/title/00040130 for 00002d* ...")
        local ok, list = pcall(fs.list_dir, "1:/title/00040130")
        if ok then
            for _, e in ipairs(list) do
                if e.type == "dir" and string.sub(e.name, 1, 6) == "00002d" then
                    total = total + dump_title("1:",
                        "00040130/" .. e.name, "nwm_" .. e.name)
                end
            end
        else
            note("  scan failed: " .. tostring(list))
        end
    end

    note("done: " .. tostring(total) .. " .code file(s) written")
end

-- Write a small manifest so the PC side knows what happened.
local manifest = table.concat(report, "\n") .. "\n"
local ok, err = pcall(fs.write_file, OUTDIR .. "/README.txt", 0, manifest)
if not ok then
    print("could not write README.txt: " .. tostring(err))
end

ui.echo(table.concat(report, "\n") ..
        "\n \nNext: copy the .code file(s) to the PC and run\n" ..
        "port/scripts/nwm-extract.py on them.")
