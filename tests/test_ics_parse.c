// tests/test_ics_parse.c —— 日历订阅解析:时区、全天、过期过滤、排序、折行、截断。
#include "ics_parse.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void feed(ics_state_t *st, const char *text)
{
    char line[256];
    const char *p = text;
    while (*p) {
        const char *e = strpbrk(p, "\r\n");
        const size_t n = e ? (size_t)(e - p) : strlen(p);
        memcpy(line, p, n);
        line[n] = '\0';
        ics_line(st, line);
        p += n;
        while (*p == '\r' || *p == '\n') p++;
    }
}

int main(void)
{
    // 现在:2026-11-19 10:00(东八区本地时间)
    const int64_t now = ics_minutes(2026, 11, 19, 10, 0);
    ics_state_t st;
    ics_init(&st, now, 480, 7);
    feed(&st,
         "BEGIN:VCALENDAR\r\n"
         "BEGIN:VEVENT\r\nDTSTART:20261119T010000Z\r\nSUMMARY:早会(已过)\r\nEND:VEVENT\r\n"
         "BEGIN:VEVENT\r\nDTSTART;TZID=Asia/Shanghai:20261119T140000\r\nSUMMARY:产品评审\r\nEND:VEVENT\r\n"
         "BEGIN:VEVENT\r\nDTSTART:20261119T040000Z\r\nSUMMARY:午饭\\, 和小王\r\nEND:VEVENT\r\n"
         "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261119\r\nSUMMARY:妈妈生日\r\nEND:VEVENT\r\n"
         "BEGIN:VEVENT\r\nDTSTART;VALUE=DATE:20261220\r\nSUMMARY:太远了\r\nEND:VEVENT\r\n"
         "BEGIN:VEVENT\r\nDTSTART:20261121T020000Z\r\nSUMMARY:一个很长很长的标题会被折\r\n"
         " 行续上\r\nEND:VEVENT\r\n"
         "END:VCALENDAR\r\n");
    assert(st.count == 3);
    // 最早的三个:今天全天(00:00)、午饭 12:00、产品评审 14:00
    assert(!strcmp(st.events[0].summary, "妈妈生日") && st.events[0].all_day);
    assert(!strcmp(st.events[1].summary, "午饭, 和小王"));
    assert(st.events[1].start == ics_minutes(2026, 11, 19, 12, 0));   // UTC 04:00 → 本地 12:00
    assert(!strcmp(st.events[2].summary, "产品评审"));
    assert(st.events[2].start == ics_minutes(2026, 11, 19, 14, 0));

    // 只有一个时把折行拼接起来
    ics_init(&st, now, 480, 7);
    feed(&st, "BEGIN:VEVENT\nDTSTART:20261121T020000Z\nSUMMARY:一个很长很长的标题会被折\n 行续上\nEND:VEVENT\n");
    assert(st.count == 1 && !strcmp(st.events[0].summary, "一个很长很长的标题会被折行续上"));

    // 缺 DTSTART 或 SUMMARY 的事件忽略;事件外的行忽略
    ics_init(&st, now, 480, 7);
    feed(&st, "SUMMARY:游离\nBEGIN:VEVENT\nSUMMARY:没时间\nEND:VEVENT\nBEGIN:VEVENT\nDTSTART:20261120T000000Z\nEND:VEVENT\n");
    assert(st.count == 0);

    // 超长中文标题按字符边界截断,不出现半个字
    ics_init(&st, now, 480, 7);
    char big[400] = "BEGIN:VEVENT\nDTSTART:20261120T000000Z\nSUMMARY:";
    for (int i = 0; i < 30; i++) strcat(big, "长");
    strcat(big, "\nEND:VEVENT\n");
    feed(&st, big);
    assert(st.count == 1 && strlen(st.events[0].summary) % 3 == 0 && strlen(st.events[0].summary) <= 47);
    puts("ics parse tests: PASS");
    return 0;
}
