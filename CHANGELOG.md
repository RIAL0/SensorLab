# Changelog

## 1.8

- **Join a WiFi network** (`J` in WiFi Scan) with its password: connection details, connectivity
  test, and a list of the devices on the network.
- **Connect to a BLE device** (`G` in BLE Scan): lists services and characteristics and reads every
  readable value, with optional passkey pairing.
- BLE devices without a broadcast name get better labels: AirPods and Beats models, Swift Pair
  names, Windows and Xbox device types, Fast Pair model codes, services and appearance.
- Sound Player: automatic level for quiet recordings, manual gain (`G`), higher default volume.
- Mic Spectrum: default gain raised to +24 dB.
- File Manager and Sound Player lists wrap around from top to bottom.
- Signal Hunt: scale labels stay visible; long names no longer run into the next column.
- File Manager action menu shows all entries at once.

## 1.7 - first public release

The first public release, with 16 tools: WiFi Scan, WiFi Monitor, BLE Scan, Signal Hunt, Mic Spectrum,
Sound Player, IMU Motion, File Manager, Text Editor, IR + LED, Battery, Chip Info, GPIO Pins, I2C Scan,
Keys + Speaker and Screen.

New since 1.6:

- **Signal Hunt** tool: lock on to one WiFi access point or BLE device and follow its signal with a
  large reading, warmer / colder indicator, history graph and Geiger-style beeps.
- File Manager: full SD card report (identification registers, filesystem, space).

## 1.6

- SD Card tool replaced by a full **File Manager**: type icons, open by type, copy / cut / paste,
  rename, new folder, new text file, recursive delete, information, hex view.
- New **Text Editor** for plain-text and Markdown files, opened from the File Manager or on its own.
- Key handling reports the key that was just pressed when several are held (fast typing).

## 1.5

- New **Sound Player** for WAV files on the SD card, including recordings from the Mic tool.

## 1.4

- Main menu reordered: radio tools first, then sensors, then system information.

## 1.3

- Detail pages wrap long values instead of clipping them.
- BLE "connectable" flag derived from the advertising PDU type.
- GPIO tool marks open-drain pins; unused expansion-header pins are parked as inputs at boot.

## 1.2

- Microphone gain moved to the ES8311 codec's analogue amplifier (0 to +30 dB).
- Peak frequency readout ignores rumble below 60 Hz; spectrogram labels moved to a gutter.
- Header and Battery screen share one percentage estimate.
- Chip Info shows the real chip revision and app-slot usage.

## 1.1

- Large-icon main menu.
- WiFi scanner rewritten to scan one channel at a time and merge results.
- Battery screen shows a charging / on battery / full indicator.

## 1.0

- First build: mic spectrum and recorder, IMU, WiFi scan and monitor, BLE scan, battery, GPIO map,
  chip info, I2C scan, SD browser, keyboard and speaker test, IR and LED test, screen test.
