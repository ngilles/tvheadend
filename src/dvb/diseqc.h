#ifndef __DISEQC_H__
#define __DISEQC_H__

#include <stdint.h>
#include <linux/dvb/frontend.h>

/**
 *   set up the switch to position/voltage/tone
 */
int diseqc_send_msg(int fe_fd, uint8_t framing_byte, uint8_t address, uint8_t cmd,
                    uint8_t data_1, uint8_t data_2, uint8_t data_3, uint8_t msg_len);
int diseqc_setup(int fe_fd, int lnb_num, int voltage, int band,
                  uint32_t version, uint32_t repeats);
int diseqc_voltage_off(int fe_fd);

#endif
