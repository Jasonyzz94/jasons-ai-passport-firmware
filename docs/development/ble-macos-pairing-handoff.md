<p align="right">
  <a href="ble-macos-pairing-handoff.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# macOS BLE HID pairing handoff

## Baseline and scope

This handoff applies to commit `ba8a9ab183d680ace53131de7062c12a53e953dd` on branch `ccr-57d53a6a-wml6dc`. That revision was built, flashed with a partition-compatible segmented workflow, and verified on-device as application version `ba8a9ab`. NVS was not erased.

- Build: PASS, including the merged-image and debug-archive checks.
- Host tests: PASS. Server Python tests were skipped because their dependencies were not installed in the build environment.
- Device tests: firmware boot and BLE diagnosis completed; full application acceptance remains pending.
- At that baseline no pairing fix had been applied. The follow-up change implements the items under [Required implementation](#required-implementation): `CONFIG_BT_NIMBLE_SM_LVL=2`, peripheral-initiated security with the decision policy in `main/ble_sec.c` (host-tested by `tests/test_ble_sec.c`), the GAP name and appearance set after HID initialization, and the DIS model number `Passport`. The remote page reports “connected” only after the link is encrypted. Build and host tests pass; [Device acceptance](#device-acceptance) on macOS and a phone is still pending.

The relay-server discovery and authentication work is a separate issue and is not covered by this BLE handoff.

## Observed behavior

The remote-control page starts NimBLE as a connectable BLE HID keyboard. macOS CoreBluetooth receives advertisements for `Passport-10C0` with the HID service (`0x1812`) and strong RSSI. This rules out a missing advertiser, an unsupported connectable mode, and a simple range problem.

The macOS Bluetooth daemon log shows that a connection from Bluetooth Settings completed successfully. GATT discovery also completed, the device was recognized as a BLE keyboard, and the system subscribed to HID report characteristics. Button presses produced GATT notifications from the device.

Despite that progress, macOS reported `lePaired=0` and encryption disabled. The connection was later disconnected by the remote device, and temporary security keys were discarded. Bluetooth Settings could consequently remain at “Connecting” instead of reaching a bonded, reconnectable HID device.

**A completed GATT connection and HID notifications do not prove that pairing, encryption, bonding, or end-to-end keyboard acceptance completed.** Do not use `retrieveConnectedPeripherals` alone to diagnose this system-owned HID connection; use the Bluetooth daemon timeline and real key input as well.

## Root causes

### 1. HID security requirements are not enabled

`sdkconfig.defaults` enables secure connections and bonding-related runtime behavior but does not set `CONFIG_BT_NIMBLE_SM_LVL`. ESP-IDF therefore uses the NimBLE default level `0`.

The NimBLE HID service adds encrypted read/write flags only when `BLE_SM_LVL` is `2` or `3`. With level `0`, macOS can discover services and subscribe without being required to pair. This matches the observed unencrypted, unbonded connection.

### 2. The application does not initiate security on connection

The successful `BLE_GAP_EVENT_CONNECT` path in [esp_ble.c](../../main/esp_ble.c) does not call `ble_gap_security_initiate()`. It only restarts advertising after a failed connection. A phone may initiate security on its own, while macOS can wait for an encryption requirement. The implementation must not rely on one central's behavior.

### 3. HID initialization resets the GAP device name

The application sets the GAP name before `esp_hidd_dev_init()`. ESP-IDF's NimBLE HID initialization subsequently calls `ble_svc_gap_init()`, restoring the default name `nimble`. The advertisement still contains `Passport-10C0`, so macOS can show different names for the same device.

The Device Information Service also reported the model number `NimBLE`. Review that value separately if the product should not expose the stack default anywhere in system device details.

## Required implementation

1. Set `CONFIG_BT_NIMBLE_SM_LVL=2` for encrypted Just Works pairing without MITM. Preserve secure connections, bonding, and NimBLE NVS persistence.
2. On a successful GAP connection, initiate security when the link is not already encrypted. Handle errors and pairing failures explicitly, avoid repeated initiation, and retain the existing repeat-pairing recovery behavior.
3. Set the GAP device name after HID/GAP service initialization and before advertising starts. Set an intentional DIS model number if required. Verify both the advertised local name and the name read by macOS.
4. Add focused host coverage for connection-state decisions where practical. Do not treat host mocks as proof of macOS pairing.
5. Keep the existing single-connection, memory, Wi-Fi-yield, and stop/restart lifecycle constraints. Run the complete repository gate before flashing.

## Device acceptance

- Remove stale host pairings, then pair from macOS Bluetooth Settings. Confirm an encrypted, bonded connection rather than only GATT readiness.
- Verify real Mac input for navigation, slide, media, modifier, and text actions as applicable. A serial `GATT notify` alone is not acceptance.
- Exit and reopen the remote-control page, reboot Passport, and confirm reconnection without a duplicate or stale `nimble` record.
- Forget the device on one host and pair again to exercise repeat-pairing recovery. Regression-test pairing and input on the phone as well.
- Confirm that `Passport-10C0` is used consistently in advertising and system device details.
- Use a partition-compatible segmented flash when retaining NVS, and verify that saved Wi-Fi profiles still work after the update.

## Environment note

The test Mac had critically low free storage during diagnosis. Ensure adequate free space before evaluating pairing-record persistence. Raw logs can contain saved network names, addresses, and local user details; keep them local or sanitize them before sharing.
