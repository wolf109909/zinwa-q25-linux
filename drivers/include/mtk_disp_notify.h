#ifndef _MTK_DISP_NOTIFY_H
#define _MTK_DISP_NOTIFY_H
#include <linux/notifier.h>
enum {
	MTK_DISP_EARLY_EVENT_BLANK = 0,
	MTK_DISP_EVENT_BLANK = 1,
	MTK_DISP_BLANK_UNBLANK = 0,
	MTK_DISP_BLANK_POWERDOWN = 1,
};
static inline int mtk_disp_notifier_register(const char *source, struct notifier_block *nb)
{
	return 0;
}
static inline int mtk_disp_notifier_unregister(struct notifier_block *nb)
{
	return 0;
}
#endif
