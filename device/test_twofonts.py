import lvgl as lv
lv.init()
import lv_config_90 as lv_config

import fs_driver
fs_font_driver = lv.fs_drv_t()
fs_driver.fs_register(fs_font_driver, 'S', 0)

font14 = lv.binfont_create('S:/fonts/kr14.bin')
print('kr14:', font14)
font20 = lv.binfont_create('S:/fonts/kr20.bin')
print('kr20:', font20)

scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x000000), 0)

def mk(text, y, font, col):
    lab = lv.label(scr)
    lab.set_text(text)
    lab.set_style_text_font(font, 0)
    lab.set_style_text_color(lv.color_make(*col), 0)
    lab.align(lv.ALIGN.TOP_LEFT, 20, y)

mk("T1-가나다", 10, font14, (255, 0, 0))
mk("T2-가나다", 60, font20, (0, 255, 0))
mk("N1-4103", 110, font14, (0, 200, 255))
mk("N2-4103", 160, font20, (255, 255, 0))
mk("한글 ABC 123", 210, font14, (255, 255, 255))
mk("경남아너스빌", 260, font20, (255, 140, 0))

print("built, pumping 15s")
import time
t0 = time.time()
while time.time() - t0 < 15:
    lv.timer_handler()
    time.sleep(0.05)
print("done")
