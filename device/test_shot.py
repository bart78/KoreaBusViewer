import lvgl as lv
log = open("prog.log", "w")
log.write("start\n"); log.flush()
lv.init()
log.write("lv init\n"); log.flush()
import lv_config_90 as lv_config
log.write("display init\n"); log.flush()

scr = lv.screen_active()
scr.set_style_bg_color(lv.color_hex(0x000000), 0)
lab = lv.label(scr)
lab.set_text("SnapshotTest 123")
lab.set_style_text_color(lv.color_make(255, 255, 255), 0)
lab.center()
log.write("ui built\n"); log.flush()

b = bytearray(scr.get_width() * scr.get_height() * 2)
log.write("bytearray ok %d\n" % len(b)); log.flush()

img = lv.image_dsc_t()
log.write("img dsc ok\n"); log.flush()

res = lv.snapshot_take_to_buf(scr, lv.COLOR_FORMAT.RGB565, img, b, len(b))
log.write("snapshot res: %s\n" % (res,)); log.flush()

with open("screen.raw", "wb") as f:
    f.write(b)
log.write("saved\n"); log.flush()
log.close()
