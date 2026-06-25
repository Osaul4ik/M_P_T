# Windows Precision Touchpad Implementation for Apple MacBook Pro 2019 16'

# About Fork
This build is primarily developed and tuned for personal use on MacBook Pro 2019 16".
It reflects behavior and fixes based on testing on this specific hardware, and may be broken or not fully optimized for other devices.

# Install Note: 
* Uninstall old touchpad driver
* Install new
  
Requires Windows running in Test Mode (driver signature enforcement disabled).

## Also Uninstallation (extremely important for reinstallation `Trackpad++` and such)

See also [here](https://magicutilities.net/magic-trackpad/help/mac-precision-touchpad-driver-installed).

1. Go to device manager
2. Find the "Apple Precision Touch Device", "Apple Multi-touch Trackpad HID filter" and "Apple Multi-touch Auxiliary Services"
3. Right click "remove the device" and also check "uninstall driver"
4. Rescan devices

## Roadmap

- [x] Add palm detection for MBP2019
- [x] Fix scroll for MBP2019
- [x] Fix cursor jump after gestures
- [ ] Add palm detection for MBA 2015
- [ ] Fix ghost tap for MBA 2015
- [?] Adjust palm detection & scroll sensivity (Little More)
- [ ] Code review
- [ ] Optimization
- [ ] Sign?

## Main Project
[mac-precision-touchpad](https://github.com/imbushuo/mac-precision-touchpad)

 
## License

- USB driver is licensed under [GPLv2](LICENSE-GPL.md).
- SPI driver is licensed under [MIT](LICENSE-MIT.md).

