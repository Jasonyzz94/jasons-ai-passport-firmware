// main/ics_parse.c —— 日历订阅流式解析。
#include "ics_parse.h"

#include <stdio.h>
#include <string.h>

int64_t ics_minutes(int y, int m, int d, int hh, int mm)
{
    // days_from_civil
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;
    return days * 1440 + hh * 60 + mm;
}

void ics_init(ics_state_t *st, int64_t now_min, int tz_offset_min, int horizon_days)
{
    memset(st, 0, sizeof(*st));
    st->now_min = now_min;
    st->tz_offset_min = tz_offset_min;
    st->horizon_min = (int64_t)horizon_days * 1440;
}

// "DTSTART;TZID=Asia/Shanghai:20261119T140000" / "DTSTART:20261119T060000Z" / "DTSTART;VALUE=DATE:20261120"
static bool parse_start(ics_state_t *st, const char *line)
{
    const char *v = strchr(line, ':');
    if (!v) return false;
    v++;
    int y, mo, d, h = 0, mi = 0;
    if (sscanf(v, "%4d%2d%2d", &y, &mo, &d) != 3) return false;
    const bool has_time = v[8] == 'T' && sscanf(v + 9, "%2d%2d", &h, &mi) == 2;
    int64_t t = ics_minutes(y, mo, d, h, mi);
    if (has_time && strchr(v, 'Z')) t += st->tz_offset_min;   // UTC → 本地
    st->cur_start = t;
    st->cur_all_day = !has_time;
    st->cur_has_start = true;
    return true;
}

static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *o++ = (*p == 'n' || *p == 'N') ? ' ' : *p;
        } else {
            *o++ = *p;
        }
    }
    *o = '\0';
}

// 截到 max 字节以内,并去掉末尾不完整的多字节字符(snprintf 按字节截断可能切开汉字)。
static void utf8_clip(char *s, size_t max)
{
    size_t n = strlen(s);
    if (n > max) n = max;
    // 找到最后一个字符的起始字节,检查它是否完整
    size_t start = n;
    while (start > 0 && ((unsigned char)s[start - 1] & 0xC0) == 0x80) start--;
    if (start > 0) {
        const unsigned char lead = (unsigned char)s[start - 1];
        const size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : 4;
        if (n - (start - 1) < need) n = start - 1;
    }
    s[n] = '\0';
}

static void commit(ics_state_t *st)
{
    if (!st->cur_has_start || !st->cur_summary[0]) return;
    // 全天日程:今天的也算(结束于当天 24:00);定时日程:还没开始的才算
    const int64_t end_ref = st->cur_all_day ? st->cur_start + 1440 : st->cur_start;
    if (end_ref <= st->now_min || st->cur_start > st->now_min + st->horizon_min) return;
    ics_event_t ev = {.start = st->cur_start, .all_day = st->cur_all_day};
    snprintf(ev.summary, sizeof(ev.summary), "%s", st->cur_summary);
    // 插入有序数组,只留最早的 ICS_MAX_EVENTS 个
    int pos = st->count;
    while (pos > 0 && st->events[pos - 1].start > ev.start) pos--;
    if (pos >= ICS_MAX_EVENTS) return;
    const int last = st->count < ICS_MAX_EVENTS ? st->count : ICS_MAX_EVENTS - 1;
    for (int i = last; i > pos; i--) st->events[i] = st->events[i - 1];
    st->events[pos] = ev;
    if (st->count < ICS_MAX_EVENTS) st->count++;
}

void ics_line(ics_state_t *st, const char *line)
{
    if (!strncmp(line, "BEGIN:VEVENT", 12)) {
        st->in_event = true;
        st->cur_has_start = false;
        st->cur_summary[0] = '\0';
        return;
    }
    if (!st->in_event) return;
    if (!strncmp(line, "END:VEVENT", 10)) {
        commit(st);
        st->in_event = false;
        return;
    }
    if (!strncmp(line, "DTSTART", 7) && (line[7] == ':' || line[7] == ';')) {
        parse_start(st, line);
    } else if (!strncmp(line, "SUMMARY", 7) && (line[7] == ':' || line[7] == ';')) {
        const char *v = strchr(line, ':');
        snprintf(st->cur_summary, sizeof(st->cur_summary), "%s", v ? v + 1 : "");
        unescape(st->cur_summary);
        utf8_clip(st->cur_summary, sizeof(st->cur_summary) - 1);
    } else if (line[0] == ' ' && st->cur_summary[0] && strlen(st->cur_summary) < sizeof(st->cur_summary) - 2) {
        // 续行:只拼接到标题上(其他字段不需要)
        const size_t n = strlen(st->cur_summary);
        snprintf(st->cur_summary + n, sizeof(st->cur_summary) - n, "%s", line + 1);
        utf8_clip(st->cur_summary, sizeof(st->cur_summary) - 1);
    }
}
