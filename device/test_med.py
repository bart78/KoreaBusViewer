import lvgl as lv
lv.init()
import lv_config_90 as lv_config

import fs_driver
fs_font_driver = lv.fs_drv_t()
fs_driver.fs_register(fs_font_driver, 'S', 500)

font = lv.binfont_create('S:/fonts/kr14.bin')
print('font:', font)

scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x000000), 0)

def mk(text, y, col):
    lab = lv.label(scr)
    lab.set_text(text)
    lab.set_style_text_font(font, 0)
    lab.set_style_text_color(lv.color_make(*col), 0)
    lab.align(lv.ALIGN.TOP_LEFT, 20, y)

mk("가나다라마", 10, (255, 0, 0))
mk("경남아너스빌", 50, (0, 255, 0))
mk("한글 ABC 123", 90, (0, 200, 255))
mk("첫차 3분후 도착", 130, (255, 255, 0))
mk("곧 도착 · 출발대기", 170, (255, 255, 255))

print("built, pumping 15s")
import time
t0 = time.time()
while time.time() - t0 < 15:
    lv.timer_handler()
    time.sleep(0.05)
print("done")
