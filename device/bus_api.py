import re, time
from http import http_get

class BusApiError(Exception):
    pass

HOST = "ws.bus.go.kr"
PORT = 80

ARRIVAL_RE = re.compile(r"(\d+)\s*분(?:\s*(\d+)\s*초)?")


def parse_arrmsg(msg):
    raw = (msg or "").strip()
    m = ARRIVAL_RE.search(raw)
    secs = None
    if m:
        secs = int(m.group(1)) * 60 + int(m.group(2) or 0)
    elif "곧" in raw or "도착" in raw:
        secs = 0
    elif "출발대기" in raw:
        secs = -1
    elif "운행종료" in raw or "종료" in raw:
        secs = -2
    return {"secs": secs, "raw": raw}


def _fetch_xml(path, key):
    import keyutil
    body = http_get(HOST, PORT, path % key, timeout=10)[1]
    txt = body.decode("utf-8", "ignore")
    m = re.search(r"<headerMsg>(.*?)</headerMsg>", txt)
    if m and ("NOT REGISTERED" in m.group(1) or "인증실패" in m.group(1)):
        body = http_get(HOST, PORT, path % keyutil.decode_key(key), timeout=10)[1]
        txt = body.decode("utf-8", "ignore")
    m = re.search(r"<headerMsg>(.*?)</headerMsg>", txt)
    if m and ("NOT REGISTERED" in m.group(1) or "인증실패" in m.group(1)):
        raise BusApiError("키 미등록(승인 반영 대기 중)")
    return txt


def _tag(item, name):
    m = re.search(r"<%s>([^<]*)</%s>" % (name, name), item)
    return m.group(1) if m else ""


def _items(txt):
    return re.findall(r"<itemList>(.*?)</itemList>", txt, re.S)


def fetch_route_arrival(key, st_id, route_id, ord_no):
    path = "/api/rest/arrive/getArrInfoByRoute?serviceKey=%s&stId=%s&busRouteId=%s&ord=%s" % (
        "%s", st_id, route_id, ord_no)
    txt = _fetch_xml(path, key)
    items = _items(txt)
    if not items:
        raise BusApiError("데이터 없음")
    item = items[0]
    arrs = []
    for tag_name in ("arrmsg1", "arrmsg2"):
        v = _tag(item, tag_name)
        if v:
            arrs.append(parse_arrmsg(v))
    return {
        "route": _tag(item, "rtNm") or _tag(item, "busRouteAbrv"),
        "dest": _tag(item, "stationNm1") or _tag(item, "sectNm"),
        "bus_type": _tag(item, "routeType"),
        "arrivals": arrs,
    }


def discover_stop_routes(key, st_id):
    """All routes serving st_id, with ord, via stationinfo getRouteByStation."""
    path = "/api/rest/stationinfo/getRouteByStation?serviceKey=%s&stationId=%s" % ("%s", st_id)
    txt = _fetch_xml(path, key)
    out = []
    for item in _items(txt):
        rid = _tag(item, "busRouteId")
        name = _tag(item, "busRouteAbrv") or _tag(item, "busRouteNm")
        ord_no = _tag(item, "staOrd") or _tag(item, "stationOrd")
        if rid and name:
            out.append({"busRouteId": rid, "name": name,
                        "ord": int(ord_no) if ord_no else None})
    if not out:
        raise BusApiError("노선 목록 없음")
    return out


def search_route(key, route_no):
    """Find busRouteId by route number via busRouteInfo getRouteList."""
    path = "/api/rest/busRouteInfo/getRouteList?serviceKey=%s&strSrch=%s" % ("%s", route_no)
    txt = _fetch_xml(path, key)
    out = []
    for item in _items(txt):
        rid = _tag(item, "busRouteId")
        nm = _tag(item, "busRouteNm")
        if rid and nm:
            out.append({"busRouteId": rid, "name": nm,
                        "kind": _tag(item, "routeType")})
    return out


def discover_route(key, st_id, route_id):
    """Find ord of st_id on a route via getArrInfoByRouteAll (in-scope function)."""
    path = "/api/rest/arrive/getArrInfoByRouteAll?serviceKey=%s&busRouteId=%s" % ("%s", route_id)
    txt = _fetch_xml(path, key)
    for item in _items(txt):
        if _tag(item, "stId") == st_id:
            return {
                "ord": _tag(item, "staOrd") or _tag(item, "sectOrd"),
                "rtNm": _tag(item, "rtNm") or _tag(item, "busRouteAbrv"),
                "sectNm": _tag(item, "sectNm"),
            }
    raise BusApiError("정류소가 노선에 없음")


def fetch_stop(key, st_id, routes):
    result = []
    for r in routes:
        try:
            info = fetch_route_arrival(key, st_id, r["busRouteId"], r["ord"])
            if not info["route"]:
                info["route"] = r["name"]
            if not info["dest"]:
                info["dest"] = r.get("dest", "")
            info["kind"] = r.get("kind", "")
            result.append(info)
        except BusApiError:
            result.append({"route": r["name"], "dest": r.get("dest", ""),
                           "kind": r.get("kind", ""), "arrivals": [], "error": True})
    return result


def demo_data():
    routes = [
        {"route": "4103", "dest": "서울역버스환승센터", "kind": "경기",
         "arrivals": [{"secs": 4 * 60 + 10, "raw": "4분 10초후"},
                      {"secs": 21 * 60 + 30, "raw": "21분 30초후"}]},
        {"route": "9409", "dest": "신사역", "kind": "광역",
         "arrivals": [{"secs": 0, "raw": "곧 도착"},
                      {"secs": 13 * 60 + 45, "raw": "13분 45초후"}]},
        {"route": "340", "dest": "영원무역", "kind": "성남",
         "arrivals": [{"secs": 8 * 60 + 20, "raw": "8분 20초후"},
                      {"secs": -1, "raw": "출발대기"}]},
    ]
    return {"stop_name": "경남아너스빌.서판교성당", "routes": routes}
