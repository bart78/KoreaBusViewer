import lvgl as lv
lv.init()
import lv_config_90 as lv_config

import fs_driver

fs_drive_letter = 'S'
fs_font_driver = lv.fs_drv_t()
fs_driver.fs_register(fs_font_driver, fs_drive_letter)

font_custom_icons = lv.binfont_create(f'{fs_drive_letter}:/assets/custom_icon_font_20.bin')
print('ICON FONT:', font_custom_icons)

font_kr = lv.binfont_create(f'{fs_drive_letter}:/fonts/kr14.bin')
print('KR FONT:', font_kr)
