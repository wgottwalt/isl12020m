This is the main development repository for the rtc-isl12020m i2c driver. It
will be pushed upstream when ready.

This driver supports the Renesas ISL12020M(IRZ) chip, which has some additional
features important for high precission and high reliability scenarios.

This driver is made for the new I2C API, which started with kernel 6.3. You can
use this driver with older kernels if you change the probe() function signature.
You can use ".probe_new" (added in kernel 4.10) instead of ".probe" in the
i2c_driver structure.

supported features:
- basic rtc functionality
- hwmon temperature (current, min, max, criticals)
- temperature and voltage drift correction (partly)
- reading of failure points, category and dates/times (partly)
- wave-gen on IRQ/F_OUT line

todo:
- alarm
