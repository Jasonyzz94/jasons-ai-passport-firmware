<p align="right">
  <a href="ble-macos-pairing-handoff.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# macOS BLE HID pairing handoff

## Baseline and scope

The original failure was diagnosed at commit `ba8a9ab183d680ace53131de7062c12a53e953dd` on branch `ccr-57d53a6a-wml6dc`. The follow-up fix is commit `0e716653cd9389ed216c55b97c57f40931f47a51` on branch `ccr-57553504-kbc89i`, directly based on the handoff commit `2288d875c865663cae9c1da610e1dcbaa276d845`.

The fix was built and flashed with a partition-compatible segmented workflow. NVS was not erased, and the device boot log confirmed application version `0e71665`.

- Build: PASS, including the merged-image and debug-archive checks.
- Host tests: PASS, including `ble_sec`. Server Python tests were skipped because their dependencies were not installed in the build environment.
- Merged image SHA-256: `6f1dbdf5da195e3aa2dc32d39d089dc557d8b607db2bd919cdc742dd08a08d10`.
- Matching ELF SHA-256: `c881e7e58228ac603c716aa35accf60ed433c27279f7c815e0740c27dd478832`.

The relay-server discovery and authentication work is a separate issue and is not covered by this BLE handoff.

## Original failure

The remote-control page started NimBLE as a connectable BLE HID keyboard. macOS received advertisements for `Passport-10C0` with the HID service (`0x1812`) and strong RSSI. The Mac could establish a BLE connection, discover GATT services, recognize a keyboard, subscribe to reports, and receive notifications.

Despite that progress, macOS reported `lePaired=0` and encryption disabled. The connection was later disconnected by the remote device, and temporary security keys were discarded. Bluetooth Settings could consequently remain at “Connecting” instead of reaching a bonded, reconnectable HID device.

**A completed GATT connection and HID notifications do not prove that pairing, encryption, bonding, or end-to-end keyboard acceptance completed.** Do not use `retrieveConnectedPeripherals` alone to diagnose this system-owned HID connection; use the Bluetooth daemon timeline and real key input as well.

## Root causes

### 1. HID security requirements were not enabled

`sdkconfig.defaults` enabled secure connections and bonding-related runtime behavior but did not set `CONFIG_BT_NIMBLE_SM_LVL`. ESP-IDF therefore used the NimBLE default level `0`.

The NimBLE HID service adds encrypted read/write flags only when `BLE_SM_LVL` is `2` or `3`. With level `0`, macOS could discover services and subscribe without being required to pair.

### 2. The application did not initiate security on connection

The successful `BLE_GAP_EVENT_CONNECT` path in [esp_ble.c](../../main/esp_ble.c) did not call `ble_gap_security_initiate()`. A phone could initiate security on its own, while macOS could wait for an encryption requirement. The implementation must not rely on one central's behavior.

### 3. HID initialization reset the GAP device name

The application set the GAP name before `esp_hidd_dev_init()`. ESP-IDF's NimBLE HID initialization subsequently called `ble_svc_gap_init()`, restoring the default name `nimble`. The advertisement still contained `Passport-10C0`, so macOS could show different names for the same device. The Device Information Service also reported the model number `NimBLE`.

## Implemented fix

1. `CONFIG_BT_NIMBLE_SM_LVL=2` requires an encrypted Just Works link while preserving secure connections, bonding, and NimBLE NVS persistence.
2. The peripheral initiates security after connecting. The decision policy in [ble_sec.c](../../main/ble_sec.c) retries once after deleting a stale local bond when the peer has lost its key, and disconnects after other failures. Host coverage is in `tests/test_ble_sec.c`.
3. The GAP name and keyboard appearance are set after HID initialization. The DIS model number is `Passport`.
4. The remote page reports “connected” only after the link is encrypted.
5. The existing single-connection, memory, Wi-Fi-yield, and stop/restart lifecycle constraints remain in place.

## Confirmed device results

- The fixed firmware logged an encrypted-and-bonded status, confirming encryption and bonding rather than only GATT readiness.
- A real macOS event tap received `UpArrow` (key code 126) and `Return/Enter` (key code 36), each with both press and release events.
- The Mac also received modifier-only events for `Right Command` (key code 54) and `Right Control` (key code 62). Passport's serial log contained the corresponding HID notifications.
- The user confirmed that `Right Command` activated their voice-input workflow. `Right Control` was delivered but did not activate it; this is an application shortcut mismatch, not an HID transport failure.
- Saved settings remained present after segmented flashing. General Wi-Fi connectivity was not part of this acceptance run.

## Down-key and voice-key semantics

In programming-assistant mode, the down button defaults to the configurable voice key. With **▼ key purpose = voice key**, a short press and a long press both express the selected modifier as press and release; neither is expected to produce `DownArrow`.

- To use the down button as an arrow, set **▼ key purpose → Down ↓** on the mode-selection page.
- In programming-assistant mode, double-clicking **Up** also sends `DownArrow` while the down button is assigned to voice input.
- A long **OK** sends `Escape`; a short **OK** sends `Return/Enter`.

Voice-input activation is owned by the target operating system or application. Delivering a modifier does not guarantee activation: the app may require a hold, a double tap, a different modifier, or a combination such as a modifier plus a letter. System dictation may also be disabled independently. Match the Passport action to the target app's actual shortcut before changing firmware.

To use Right Command, choose **Voice key → Right Command** on the mode-selection page; the choice is saved in NVS. Firmware now defaults an unsaved voice key to Right Command instead of Fn, because macOS may ignore Fn from third-party keyboards. Devices with a saved choice keep it.

## Remaining acceptance

- After changing **▼ key purpose → Down ↓**, verify a real macOS `DownArrow` event.
- Exercise slide, media, modifier-plus-key, and text actions as applicable.
- Exit and reopen the remote-control page, reboot Passport, and confirm reconnection without a duplicate or stale record.
- Forget the device on one host and pair again to exercise repeat-pairing recovery. Regression-test pairing and input on the phone as well.
- Recheck the runtime GAP name and DIS model number in system device details; the code fix alone is not a display acceptance result.
- Verify actual Wi-Fi connectivity separately when a known network is available.

## Privacy note

Raw logs can contain saved network names, addresses, local user details, and application information. Keep them local or sanitize them before sharing. Do not commit firmware binaries, debug archives, credentials, or unsanitized logs with this handoff.
