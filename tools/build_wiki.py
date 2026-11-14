#!/usr/bin/env python3
"""Builds the GitHub wiki pages from README.md so the two never drift apart.

Usage: python3 tools/build_wiki.py <output-dir>
The README stays the single source for tool descriptions and key maps; this script splits it into
one wiki page per topic and adds the pages that only make sense in a wiki (Home, FAQ, sidebar).
"""
import os
import re
import sys

REPO = "RIAL0/SensorLab"
RAW = f"https://raw.githubusercontent.com/{REPO}/main/"
BLOB = f"https://github.com/{REPO}/blob/main/"

# tool title in the README -> photos to show on its wiki page
SHOTS = {
    "WiFi Scan": ["wifi-scan", "wifi-channels"],
    "WiFi Monitor": ["wifi-monitor"],
    "BLE Scan": ["ble-scan"],
    "Signal Hunt": ["signal-hunt"],
    "Mic Spectrum": ["mic-spectrum", "mic-spectrogram"],
    "Sound Player": ["sound-player"],
    "IMU Motion": ["imu-attitude", "imu-bars"],
    "File Manager": ["file-manager"],
    "Text Editor": ["text-editor"],
    "IR Sender": ["ir-sender"],
    "Battery": ["battery-charging"],
    "Chip Info": ["chip-info"],
    "GPIO Pins": ["gpio-pins"],
    "I2C Scan": ["i2c-scan"],
    "Keys + Speaker": ["keys"],
}

FAQ = """\
# FAQ and troubleshooting

## Installing

**Launcher says the file is too big, or the install fails.**
SensorLab needs about 1.4 MB of app space. If Launcher's partition layout leaves less than that,
choose a layout with a larger app partition in Launcher's settings and install again.

**How do I get back to Launcher?**
Restart the Cardputer and press a key while Launcher's start screen is showing.

**The start screen says the ADV was not detected.**
SensorLab is written for the Cardputer ADV. On the original Cardputer it still starts, but the
tools that rely on ADV-only parts (the IMU and the ES8311 audio codec) will not work.

## WiFi

**My 5 GHz network is not listed.**
The ESP32-S3 radio is 2.4 GHz only. Networks on 5 GHz or 6 GHz are invisible to it.

**"Find devices on this network" shows only the router and the Cardputer.**
That is normal on guest and public networks, which stop clients from seeing each other. The same
isolation can make the router and internet pings fail while the DNS lookup still works.

**I cannot join my office network.**
Networks that ask for a username and a password (WPA2-Enterprise) are not supported.

## Bluetooth

**Why do most devices have no name?**
A phone's Bluetooth list gets most names from Bluetooth Classic, from devices it has already
paired with, or by connecting. A BLE scanner only sees what a device broadcasts, and many devices,
including every Apple device, broadcast no name. See [[BLE Scan]] for the labels SensorLab builds
instead.

**How do I find my own device among dozens of others?**
Hold it next to the Cardputer, press `F` in [[BLE Scan]] until the *near* filter is active, then
tag it with `T`. After that, the *my tags* filter shows only the devices you named.

**My tag disappeared after a while.**
Phones, watches and earbuds change their Bluetooth address every 15 minutes or so, on purpose.
A tag follows the address, so it is lost when the address changes. Devices with a fixed address
keep their tag permanently in `/SensorLab/ble_names.csv`.

**Signal Hunt says SIGNAL LOST.**
Either you moved out of range, or the device changed its address. Go back to the list and pick it
again.

## Audio

**My recording is very quiet.**
Raise the microphone gain with `;` in [[Mic Spectrum]] before recording. On playback, the
[[Sound Player]] brings quiet files up to full level automatically; `G` changes the gain by hand.

**There is no sound while Mic Spectrum is open.**
The microphone and the speaker share one audio bus, so the speaker is off while the microphone is
in use.

**A WAV file will not play.**
The player handles uncompressed PCM, 8 or 16 bit, mono or stereo. MP3 files, compressed WAV and
24 or 32 bit WAV are not supported.

## Files and text

**How do I move the cursor in the Text Editor?**
Hold `Fn` and use `;` `.` `,` `/`. Without `Fn` those keys type their characters. See
[[Text Editor]].

**I cannot leave the keyboard test.**
Press the **G0** button on the side. Every key is under test there, so none of them can exit.

**Files created on the Cardputer have no date.**
The Cardputer has no real-time clock, so SensorLab cannot stamp files with the time.

## Battery

**The percentage jumps around or seems wrong.**
The ADV has no fuel-gauge chip. The percentage is estimated from the cell voltage, which moves with
load and while charging.

**It takes a while to show CHARGING.**
With a computer attached, charging is detected straight away from the USB link. With a plain
charger it is inferred from the voltage rising, which can take around 30 seconds.
"""


def slug(title):
    return re.sub(r"\s+", "-", title.replace("+", "and").strip())


def wikilinks(text, pages):
    """Rewrite README anchors and repo-relative links for the wiki."""
    def anchor(m):
        label, target = m.group(1), m.group(2).lstrip("#")
        for title in pages:
            if re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-") in target:
                return f"[[{label}|{slug(title)}]]"
        return label
    text = re.sub(r"\[([^\]]+)\]\((#[^)]+)\)", anchor, text)
    text = re.sub(r"\]\((?!https?://|#)([^)]+)\)", lambda m: f"]({BLOB}{m.group(1)})", text)
    text = re.sub(r'src="docs/', f'src="{RAW}docs/', text)
    return text


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "wiki-out"
    os.makedirs(out, exist_ok=True)
    readme = open("README.md", encoding="utf-8").read()

    # split the README into "## " sections
    parts = re.split(r"^## ", readme, flags=re.M)
    intro, sections = parts[0], {}
    for p in parts[1:]:
        title, _, body = p.partition("\n")
        sections[title.strip()] = body.strip() + "\n"

    # split "Tools" into one entry per "### N. Name"
    tools = []
    for t in re.split(r"^### ", sections["Tools"], flags=re.M)[1:]:
        title, _, body = t.partition("\n")
        num, name = title.split(". ", 1)
        tools.append((int(num), name.strip(), body.strip() + "\n"))
    tool_names = [n for _, n, _ in tools]

    topic_pages = {
        "Installation": sections["Hardware"] and ("## Hardware\n\n" + sections["Hardware"] + "\n## Install\n\n" + sections["Install"]),
        "Controls": sections["Controls"],
        "Files on the SD card": sections["Files on the SD card"],
        "Hardware limits": sections["Hardware limits"],
        "Building from source": sections["Build from source"],
        "Responsible use": sections["Responsible use"],
    }
    all_titles = tool_names + list(topic_pages) + ["Install", "Build from source"]

    def write(title, body):
        with open(os.path.join(out, slug(title) + ".md"), "w", encoding="utf-8") as f:
            f.write(wikilinks(body, all_titles))

    for title, body in topic_pages.items():
        write(title, f"# {title}\n\n{body}")

    for i, (num, name, body) in enumerate(tools):
        shots = "".join(f'<img src="{RAW}docs/screens/{s}.jpg" width="360" alt="{name}"> ' for s in SHOTS.get(name, []))
        nav = []
        if i > 0:
            nav.append(f"[[< {tools[i - 1][1]}|{slug(tools[i - 1][1])}]]")
        nav.append("[[All tools|Home#tools]]")
        if i + 1 < len(tools):
            nav.append(f"[[{tools[i + 1][1]} >|{slug(tools[i + 1][1])}]]")
        page = f"# {name}\n\nTool {num} of {len(tools)} in the main menu.\n\n"
        if shots:
            page += shots.strip() + "\n\n"
        page += body + "\n---\n" + " &nbsp;|&nbsp; ".join(nav) + "\n"
        write(name, page)

    first_line = re.search(r"^\*\*(.+?)\*\*", intro, flags=re.M).group(1)
    summary = intro.split(first_line + "**", 1)[1].split("![", 1)[0].strip()
    home = f"# SensorLab\n\n**{first_line}**\n\n{summary}\n\n"
    home += f'<img src="{RAW}docs/menu-16-tools.png" width="720" alt="SensorLab main menu">\n\n'
    home += "## Start here\n\n"
    home += "- [[Installation]]: what you need and how to install with M5Launcher\n"
    home += "- [[Controls]]: the keys that work everywhere\n"
    home += "- [[FAQ]]: common questions and fixes\n"
    home += f"- [Latest release](https://github.com/{REPO}/releases/latest) and [changelog]({BLOB}CHANGELOG.md)\n\n"
    home += "## Tools\n\n| # | Tool | What it does |\n|---|---|---|\n"
    for num, name, body in tools:
        first = re.split(r"(?<=[.!?])\s", " ".join(body.split("\n\n")[0].split()))[0]
        first = first.split(":")[0].rstrip(".") + "."          # keep the table to one short line per tool
        home += f"| {num} | [{name}](https://github.com/{REPO}/wiki/{slug(name)}) | {first} |\n"   # plain links: [[a|b]] would break the table
    home += "\n## Reference\n\n"
    home += "- [[Files on the SD card|Files-on-the-SD-card]]\n- [[Hardware limits|Hardware-limits]]\n"
    home += "- [[Building from source|Building-from-source]]\n- [[Responsible use|Responsible-use]]\n"
    write("Home", home)
    write("FAQ", FAQ)

    side = "**[[Home]]**\n\n**Getting started**\n- [[Installation]]\n- [[Controls]]\n- [[FAQ]]\n\n**Tools**\n"
    side += "".join(f"- [[{n}|{slug(n)}]]\n" for n in tool_names)
    side += "\n**Reference**\n- [[SD card files|Files-on-the-SD-card]]\n- [[Hardware limits|Hardware-limits]]\n"
    side += "- [[Building|Building-from-source]]\n- [[Responsible use|Responsible-use]]\n"
    open(os.path.join(out, "_Sidebar.md"), "w", encoding="utf-8").write(side)
    open(os.path.join(out, "_Footer.md"), "w", encoding="utf-8").write(
        f"Generated from the [README]({BLOB}README.md). To change a page, edit the README or `tools/build_wiki.py` in the repository.\n")
    print(f"wrote {len(os.listdir(out))} pages to {out}")


if __name__ == "__main__":
    main()
