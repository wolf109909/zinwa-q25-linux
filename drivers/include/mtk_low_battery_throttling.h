#ifndef _MTK_LOW_BATTERY_THROTTLING_H
#define _MTK_LOW_BATTERY_THROTTLING_H
enum LOW_BATTERY_LEVEL_TAG {
	LOW_BATTERY_LEVEL_0 = 0,
	LOW_BATTERY_LEVEL_1 = 1,
	LOW_BATTERY_LEVEL_2 = 2,
	LOW_BATTERY_LEVEL_NUM = 3,
};
typedef void (*low_battery_callback)(enum LOW_BATTERY_LEVEL_TAG tag);
static inline int register_low_battery_notify(low_battery_callback cb, int prio) { return 0; }
#endif
