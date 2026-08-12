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

def mk(text, y, col=(255, 255, 255)):
    lab = lv.label(scr)
    lab.set_text(text)
    lab.set_style_text_font(font_kr, 0)
    lab.set_style_text_color(lv.color_make(*col), 0)
    lab.align(lv.ALIGN.TOP_LEFT, 20, y)
    return lab

mk("ABC 123 456", 10, (255, 0, 0))
mk("가나다라마", 40, (0, 255, 0))
mk("경남아너스빌", 70, (0, 200, 255))
mk("첫차 3분후 도착", 100, (255, 255, 0))
mk(b"bytes: 가나다", 130, (255, 255, 255))

print("done")
