/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Zinwa Q25: MT6358/MT6366 PMIC accessory (headset jack) detection.
 */
#ifndef _Q25_MT6358_ACCDET_H
#define _Q25_MT6358_ACCDET_H

struct snd_soc_jack;

int q25_mt6358_accdet_set_jack(struct snd_soc_jack *jack);
int q25_mt6358_accdet_register(void);
void q25_mt6358_accdet_unregister(void);

#endif
