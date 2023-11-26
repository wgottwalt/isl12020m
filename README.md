This is the main development repository for the rtc-isl12020m i2c driver which
also will be mainlined when ready.

This driver supports the Renesas ISL12020MIRZ chip (or short: isl12020m), which
has some quite unique features important for high precission and high
reliability scenarios.

supported features:
- basic rtc functionality

todo:
- hwmon temperaturs
- temperature and voltage drift correction
- reading of failure points, category and dates/times
- wave-gen on IRQ/F_OUT line (connect it to a GPIO, gen 1Hz and use it as PPS)
- alarm
- 128 bytes sram access (battery buffered)
