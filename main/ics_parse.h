// main/ics_parse.h —— 日历订阅(.ics)流式解析:逐行喂入,只保留接下来几天里最近的几个日程。
// 纯逻辑,主机测试覆盖。重复规则(RRULE)不展开,只看第一次发生的时间。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ICS_MAX_EVENTS 3

typedef struct {
    int64_t start;           // 本地时间的"分钟数"(自 1970-01-01 00:00 起,已换算时区)
    bool all_day;
    char summary[48];
} ics_event_t;

typedef struct {
    int64_t now_min;         // 当前本地时间(分钟)
    int64_t horizon_min;     // 只看 now..now+horizon 之间的日程
    int tz_offset_min;       // 本地时区相对 UTC 的偏移(东八区 = 480)
    // 解析状态
    bool in_event;
    int64_t cur_start;
    bool cur_all_day;
    bool cur_has_start;
    char cur_summary[48];
    // 结果:按开始时间升序
    ics_event_t events[ICS_MAX_EVENTS];
    int count;
} ics_state_t;

void ics_init(ics_state_t *st, int64_t now_min, int tz_offset_min, int horizon_days);
// 喂入一行(不含换行符)。折行(以空格开头的续行)只用于 SUMMARY,会被拼接。
void ics_line(ics_state_t *st, const char *line);
// "年月日时分" → 分钟数(纯计算)
int64_t ics_minutes(int year, int month, int day, int hour, int minute);
