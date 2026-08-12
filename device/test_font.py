import lvgl as lv
lv.init()
import lv_config_90 as lv_config

import fs_driver

fs_drive_letter = 'S'
fs_font_driver = lv.fs_drv_t()
fs_driver.fs_register(fs_font_driver, fs_drive_letter)

font_kr = lv.binfont_create('S:/fonts/kr14.bin')
print('KR FONT:', font_kr)

import task_handler
th = task_handler.TaskHandler()

scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x000000), 0)

lab = lv.label(scr)
lab.set_text("경남아너스빌.서판교성당")
lab.set_style_text_font(font_kr, 0)
lab.set_style_text_color(lv.color_make(255, 255, 255), 0)
lab.center()

lab2 = lv.label(scr)
lab2.set_text("첫차 3분후 도착 · 2번째 12분후")
lab2.set_style_text_font(font_kr, 0)
lab2.set_style_text_color(lv.color_make(255, 255, 255), 0)
lab2.align(lv.ALIGN.CENTER, 0, 30)

print("done")
