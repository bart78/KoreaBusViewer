import lvgl as lv
lv.init()
import lv_config as lv_config

d = lv.screen_active().get_display()
d.set_render_mode(lv.DISPLAY_RENDER_MODE.FULL)

scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x0B0E14), 0)

lab = lv.label(scr)
lab.set_pos(20, 120)
lab.set_style_text_font(lv.font_montserrat_48, 0)
lab.set_style_text_color(lv.color_make(255, 255, 0), 0)
lab.set_text("TICK 0")

lab2 = lv.label(scr)
lab2.set_pos(20, 200)
lab2.set_style_text_font(lv.font_montserrat_24, 0)
lab2.set_style_text_color(lv.color_make(0, 255, 0), 0)
lab2.set_text("running 30s")

import time
t0 = time.time()
while time.time() - t0 < 30:
    n = int(time.time() - t0)
    lab.set_text("TICK %d" % n)
    lab2.set_text("running %d s" % n)
    lv.timer_handler()
    time.sleep(0.5)
print("done")
