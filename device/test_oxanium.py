import lvgl as lv
lv.init()
import lv_config_90 as lv_config

import fs_driver
fs_font_driver = lv.fs_drv_t()
fs_driver.fs_register(fs_font_driver, 'S', 500)

font_ox = lv.binfont_create('S:/assets/oxanium16.bin')
print('oxanium:', font_ox)

scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x000000), 0)

def mk(text, y, font, col):
    lab = lv.label(scr)
    lab.set_text(text)
    lab.set_style_text_font(font, 0)
    lab.set_style_text_color(lv.color_make(*col), 0)
    lab.align(lv.ALIGN.TOP_LEFT, 20, y)

mk("OX-ABC abc 123", 10, font_ox, (255, 0, 0))
mk("OX-Lorem Ipsum", 50, font_ox, (0, 255, 0))
mk("OX-0123456789", 90, font_ox, (0, 200, 255))

print("built, pumping 15s")
import time
t0 = time.time()
while time.time() - t0 < 15:
    lv.timer_handler()
    time.sleep(0.05)
print("done")
