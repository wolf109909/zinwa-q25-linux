#ifndef _PMIC_LBAT_SERVICE_H
#define _PMIC_LBAT_SERVICE_H
struct lbat_user;
static inline struct lbat_user *lbat_user_register_ext(const char *name, unsigned int *thd_volt_arr,
						       unsigned int thd_volt_size,
						       void (*callback)(unsigned int thd_volt))
{
	return NULL;
}
static inline struct lbat_user *lbat_user_register(const char *name, unsigned int hv_thd_volt,
						   unsigned int lv1_thd_volt,
						   unsigned int lv2_thd_volt,
						   void (*callback)(unsigned int thd_volt))
{
	return NULL;
}
#endif
