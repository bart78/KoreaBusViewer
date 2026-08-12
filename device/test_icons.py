import lvgl as lv
lv.init()
import lv_config_90 as lv_config

import fs_driver

fs_drive_letter = 'S'
fs_font_driver = lv.fs_drv_t()
fs_driver.fs_register(fs_font_driver, fs_drive_letter)

font_icons = lv.binfont_create('S:/assets/custom_icon_font_20.bin')
print('ICONS:', font_icons)
font_kr = lv.binfont_create('S:/fonts/kr14.bin')
print('KR:', font_kr)

import task_handler
th = task_handler.TaskHandler()

scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x000000), 0)

def mk(text, y, font, col=(255, 255, 255), x=20):
    lab = lv.label(scr)
    lab.set_text(text)
    lab.set_style_text_font(font, 0)
    lab.set_style_text_color(lv.color_make(*col), 0)
    lab.align(lv.ALIGN.TOP_LEFT, x, y)
    return lab

mk("icons \ue900\ue901", 10, font_icons, (255, 0, 0))
mk("KR: ABC 123", 60, font_kr, (0, 255, 0))
mk("KR: 가나다라마", 110, font_kr, (0, 200, 255))
mk("montserrat: ABC", 160, lv.font_montserrat_16, (255, 255, 0))

print("done")
