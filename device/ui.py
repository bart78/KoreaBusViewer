import lvgl as lv

BG = lv.color_hex(0x0B0E14)
SEP = lv.color_hex(0x1A2130)
TXT_WHITE = (255, 255, 255)
TXT_DIM = (150, 160, 178)
TXT_GOOD = (0, 200, 83)
TXT_WARN = (255, 213, 79)
TXT_BAD = (255, 82, 82)

HEADER_H = 52
ROW_H = 46
MAX_ROWS = 7


def route_color(route):
    if not route:
        return (96, 125, 139)
    c = route[0]
    if c == "0":
        return (253, 216, 53)
    if c in "1234":
        return (41, 121, 255)
    if c in "5678":
        return (0, 200, 83)
    if c == "9":
        return (213, 0, 0)
    return (96, 125, 139)


def fmt_secs(secs):
    if secs is None:
        return ""
    if secs == -1:
        return "WAIT"
    if secs == -2:
        return "DONE"
    if secs < 60:
        return "SOON"
    m, s = divmod(secs, 60)
    return "%dm %02ds" % (m, s) if s else "%dm" % m


class Row:
    def __init__(self, scr, y, f_badge, f_small, f_big):
        self.badge = lv.obj(scr)
        self.badge.set_size(52, 32)
        self.badge.set_pos(8, y)
        self.badge.set_style_bg_color(lv.color_hex(0x37474F), 0)
        self.badge.set_style_radius(6, 0)

        self.route = lv.label(self.badge)
        self.route.set_style_text_font(f_badge, 0)
        self.route.set_style_text_color(lv.color_make(255, 255, 255), 0)
        self.route.center()

        self.kind = lv.label(scr)
        self.kind.set_pos(70, y + 1)
        self.kind.set_width(80)
        self.kind.set_style_text_font(f_small, 0)
        self.kind.set_style_text_color(lv.color_make(*TXT_DIM), 0)

        self.arr1 = lv.label(scr)
        self.arr1.set_pos(150, y - 4)
        self.arr1.set_width(160)
        self.arr1.set_style_text_font(f_big, 0)
        self.arr1.set_style_text_color(lv.color_make(*TXT_WHITE), 0)
        self.arr1.set_style_text_align(lv.TEXT_ALIGN.RIGHT, 0)
        self._a1 = None
        self._a2 = None

        self.arr2 = lv.label(scr)
        self.arr2.set_pos(150, y + 26)
        self.arr2.set_width(160)
        self.arr2.set_style_text_font(f_small, 0)
        self.arr2.set_style_text_color(lv.color_make(*TXT_DIM), 0)
        self.arr2.set_style_text_align(lv.TEXT_ALIGN.RIGHT, 0)

        self.hide()

    def hide(self):
        self.badge.add_flag(lv.obj.FLAG.HIDDEN)
        self.kind.add_flag(lv.obj.FLAG.HIDDEN)
        self.arr1.add_flag(lv.obj.FLAG.HIDDEN)
        self.arr2.add_flag(lv.obj.FLAG.HIDDEN)

    def show(self):
        self.badge.remove_flag(lv.obj.FLAG.HIDDEN)
        self.kind.remove_flag(lv.obj.FLAG.HIDDEN)
        self.arr1.remove_flag(lv.obj.FLAG.HIDDEN)
        self.arr2.remove_flag(lv.obj.FLAG.HIDDEN)

    def set_route(self, route, col):
        self.badge.set_style_bg_color(lv.color_make(*col), 0)
        bright = col[0] * 0.3 + col[1] * 0.6 + col[2] * 0.1
        self.route.set_style_text_color(
            lv.color_make(0, 0, 0) if bright > 150 else lv.color_make(255, 255, 255), 0)
        self.route.set_text(route)

    def set_kind(self, kind):
        self.kind.set_text(kind or "")

    def set_arrivals(self, s1, s2):
        t1 = fmt_secs(s1) if s1 is not None else ""
        t2 = fmt_secs(s2) if s2 is not None else ""
        if t1 != self._a1:
            self._a1 = t1
            self.arr1.set_text(t1)
        if t2 != self._a2:
            self._a2 = t2
            self.arr2.set_text(t2)


class BusUI:
    def __init__(self):
        f_small = lv.font_montserrat_14
        f_badge = lv.font_montserrat_20
        f_big = lv.font_montserrat_28
        f_hdr = lv.font_montserrat_20

        self.secs = []

        self.scr = lv.screen_active()
        self.scr.set_style_bg_color(BG, 0)

        self.stop_name = lv.label(self.scr)
        self.stop_name.set_pos(12, 8)
        self.stop_name.set_width(180)
        self.stop_name.set_style_text_font(f_hdr, 0)
        self.stop_name.set_style_text_color(lv.color_make(*TXT_WHITE), 0)
        self.stop_name.set_long_mode(lv.label.LONG.DOT)

        self.mode = lv.label(self.scr)
        self.mode.set_pos(200, 10)
        self.mode.set_width(110)
        self.mode.set_style_text_font(f_small, 0)
        self.mode.set_style_text_color(lv.color_make(*TXT_DIM), 0)
        self.mode.set_style_text_align(lv.TEXT_ALIGN.RIGHT, 0)

        self.sub = lv.label(self.scr)
        self.sub.set_pos(12, 34)
        self.sub.set_width(300)
        self.sub.set_style_text_font(f_small, 0)
        self.sub.set_style_text_color(lv.color_make(*TXT_DIM), 0)

        self.btn = lv.button(self.scr)
        self.btn.set_size(72, 30)
        self.btn.set_pos(400, 6)
        self.btn.set_style_bg_color(lv.color_hex(0x1B2432), 0)
        self.btn.set_style_radius(6, 0)
        self.btn_label = lv.label(self.btn)
        self.btn_label.set_style_text_font(f_small, 0)
        self.btn_label.set_style_text_color(lv.color_make(*TXT_DIM), 0)
        self.btn_label.set_text("REFRESH")
        self.btn_label.center()

        self.rows = []
        for i in range(MAX_ROWS):
            self.rows.append(Row(self.scr, HEADER_H + i * ROW_H + 4, f_badge, f_small, f_big))

    def on_refresh(self, cb):
        self.btn.add_event_cb(lambda e: cb(), lv.EVENT.CLICKED, None)

    def set_data(self, data, mode, note=""):
        self.secs = []
        self.stop_name.set_text(data.get("stop_name", ""))
        self.mode.set_text(mode)
        self.mode.set_style_text_color(
            lv.color_make(*TXT_WARN) if mode != "LIVE" else lv.color_make(*TXT_GOOD), 0)
        self.sub.set_text(note)
        routes = data.get("routes", [])[:MAX_ROWS]
        for i, r in enumerate(routes):
            row = self.rows[i]
            row.show()
            row.set_route(r["route"], route_color(r["route"]))
            row.set_kind(r.get("kind", ""))
            a1 = r["arrivals"][0] if r.get("arrivals") else None
            a2 = r["arrivals"][1] if r.get("arrivals") and len(r["arrivals"]) > 1 else None
            s1 = a1["secs"] if a1 else None
            s2 = a2["secs"] if a2 else None
            row.set_arrivals(s1, s2)
            d1 = time_now() + s1 if s1 is not None and s1 >= 0 else None
            d2 = time_now() + s2 if s2 is not None and s2 >= 0 else None
            self.secs.append((d1, d2, s1, s2))
        for i in range(len(routes), MAX_ROWS):
            self.rows[i].hide()

    def tick(self):
        now = time_now()
        for i, (d1, d2, s1, s2) in enumerate(self.secs):
            row = self.rows[i]
            v1 = s1 if d1 is None else max(d1 - now, 0)
            v2 = s2 if d2 is None else max(d2 - now, 0)
            row.set_arrivals(v1, v2)

    def set_night(self, clock):
        self.secs = []
        for row in self.rows:
            row.hide()
        self.mode.set_text("NIGHT")
        self.mode.set_style_text_color(lv.color_make(*TXT_DIM), 0)
        self.stop_name.set_text("SERVICE SUSPENDED")
        self.sub.set_text("%s  -  resumes 04:00" % clock)


def time_now():
    import time
    return time.time()
