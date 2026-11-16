/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Airoha AN7581 GPON MAC 寄存器定义
 *
 * 由 tools/re_xpon.py 从原厂 xpon.ko 逆向还原，请勿手工修改。
 * 偏移基准 = 设备树 "gpon" 资源块（0x1fb64000，长度 0x3e8）。
 * 原厂驱动里这些寄存器位于 g_gpon_mac_reg_BASE + 0x4000，
 * 此处已减去 0x4000，可直接配合 devm_platform_ioremap_resource_byname(pdev, "gpon") 使用。
 */
#ifndef _AIROHA_XPON_GPON_REGS_H_
#define _AIROHA_XPON_GPON_REGS_H_

#define AIROHA_GPON_ONU_ID                                   0x0000 /* ONU ID */
#define AIROHA_GPON_ONU_GLOBAL_CONFIGURATION_REGISTER        0x0004 /* ONU GLOBAL CONFIGURATION REGISTER */
#define AIROHA_GPON_GPON_MAC_INTERRUPTS_STATUS               0x0008 /* GPON MAC INTERRUPTS STATUS */
#define AIROHA_GPON_GPON_MAC_INTERRUPTS_ENABLE               0x000c /* GPON MAC INTERRUPTS ENABLE */
#define AIROHA_GPON_ALLOC_ID_T_CONT0_T_CONT1                 0x0020 /* The Alloc-ID of T-CONT0 and T-CONT1 */
#define AIROHA_GPON_ALLOC_ID_T_CONT2_T_CONT3                 0x0024 /* The Alloc-ID of T-CONT2 and T-CONT3 */
#define AIROHA_GPON_ALLOC_ID_T_CONT4_T_CONT5                 0x0028 /* The Alloc-ID of T-CONT4 and T-CONT5 */
#define AIROHA_GPON_ALLOC_ID_T_CONT6_T_CONT7                 0x002c /* The Alloc-ID of T-CONT6 and T-CONT7 */
#define AIROHA_GPON_ALLOC_ID_T_CONT8_T_CONT9                 0x0030 /* The Alloc-ID of T-CONT8 and T-CONT9 */
#define AIROHA_GPON_ALLOC_ID_T_CONT10_T_CONT11               0x0034 /* The Alloc-ID of T-CONT10 and T-CONT11 */
#define AIROHA_GPON_ALLOC_ID_T_CONT12_T_CONT13               0x0038 /* The Alloc-ID of T-CONT12 and T-CONT13 */
#define AIROHA_GPON_ALLOC_ID_T_CONT14_T_CONT15               0x003c /* The Alloc-ID of T-CONT14 and T-CONT15 */
#define AIROHA_GPON_OMCI_GEM_PORT_ID                         0x0048 /* OMCI GEM PORT ID */
#define AIROHA_GPON_GEM_PORT_INDEX                           0x0040 /* GEM PORT INDEX（逆向补充：gponDevSetGemInfo @0x01ecc8 写入点，bit31=写命令，bit16=方向，bit17=加密，低12位=端口号） */
#define AIROHA_GPON_GEM_PORT_INDEX_STATUS                    0x0044 /* GEM PORT INDEX STATUS（逆向补充：bit31=完成，bit0=结果） */
#define AIROHA_GPON_GEM_TABLE_INITIALIZATION_CONTROL         0x004c /* GEM TABLE INITIALIZATION CONTROL */
#define AIROHA_GPON_PLOAMU_FIFO_STATUS                       0x0050 /* PLOAMu FIFO STATUS */
#define AIROHA_GPON_PLOAMU_FIFO_DATA                         0x0054 /* PLOAMu FIFO 写入端口（逆向补充：gponDevSendPloamMsg @0x01df34 实际写入点） */
#define AIROHA_GPON_PLOAMD_FIFO_STATUS                       0x0058 /* PLOAMd FIFO STATUS */
#define AIROHA_GPON_PLOAMD_FIFO_DATA                         0x005c /* PLOAMd FIFO 读取端口（逆向补充：gponDevGetPloamMsg @0x01de8c 实际读取点） */
#define AIROHA_GPON_SUPER_FRAME_COUNTER_AES_KEY_CHANGE       0x0060 /* SUPER FRAME COUNTER FOR AES KEY CHANGE */
#define AIROHA_GPON_ACTIVE_AES_KEY0                          0x0064 /* ACTIVE AES KEY0 */
#define AIROHA_GPON_ACTIVE_AES_KEY1                          0x0068 /* ACTIVE AES KEY1 */
#define AIROHA_GPON_ACTIVE_AES_KEY2                          0x006c /* ACTIVE AES KEY2 */
#define AIROHA_GPON_ACTIVE_AES_KEY3                          0x0070 /* ACTIVE AES KEY3 */
#define AIROHA_GPON_AES_KEY_SHADOW_REGISTER0                 0x0074 /* AES KEY in SHADOW REGISTER0 */
#define AIROHA_GPON_AES_KEY_SHADOW_REGISTER1                 0x0078 /* AES KEY in SHADOW REGISTER1 */
#define AIROHA_GPON_AES_KEY_SHADOW_REGISTER2                 0x007c /* AES KEY in SHADOW REGISTER2 */
#define AIROHA_GPON_AES_KEY_SHADOW_REGISTER3                 0x0080 /* AES KEY in SHADOW REGISTER3 */
#define AIROHA_GPON_AES_KEY_SWITCH_BY_SOFTWARE               0x0084 /* AES KEY SWITCH BY SOFTWARE */
#define AIROHA_GPON_PHYSICAL_LAYER_OVERHEAD_TIME             0x0090 /* PHYSICAL LAYER OVERHEAD TIME */
#define AIROHA_GPON_NUMBER_GUARD_BITS                        0x0094 /* NUMBER OF GUARD BITS */
#define AIROHA_GPON_NUMBER_TYPE1_TYPE2_PREAMBLE_BITS         0x0098 /* NUMBER OF TYPE1 and TYPE2 PREAMBLE BITS */
#define AIROHA_GPON_NUNMBER_TYPE3                            0x009c /* NUNMBER OF TYPE3 */
#define AIROHA_GPON_NUMBER_DELIMETER_BITS                    0x00a0 /* NUMBER OF DELIMETER BITS */
#define AIROHA_GPON_PRE_ASSIGNED_DELAY                       0x00a4 /* PRE-ASSIGNED DELAY */
#define AIROHA_GPON_MAIN_PATH_EQUALIZATION_DELAY             0x00a8 /* MAIN PATH EQUALIZATION DELAY */
#define AIROHA_GPON_ONU_ACTIVATION_PARAMETER                 0x00ac /* ONU ACTIVATION PARAMETER */
#define AIROHA_GPON_VENDOR_ID                                0x00b0 /* Vendor ID */
#define AIROHA_GPON_VENDOR_SPECIFIC_SERIAL_NUMBER            0x00b4 /* Vendor-specific serial number */
#define AIROHA_GPON_SN_ONU_MESSAGE_INFORMATION               0x00b8 /* SN_ONU MESSAGE INFORMATION */
#define AIROHA_GPON_ONU_STATE_ACTIVATION                     0x00bc /* ONU STATE IN ACTIVATION */
#define AIROHA_GPON_SUPER_FRAME_COUNTER_TIME_DAY_CONFIGURATION 0x00d0 /* SUPER FRAME COUNTER FOR TIME OF DAY CONFIGURATION */
#define AIROHA_GPON_NEW_TOD_SECOND_LOW_32_BITS               0x00d4 /* NEW ToD SECOND LOW 32 BITS */
#define AIROHA_GPON_NEW_TOD_NANO_SECOND                      0x00d8 /* NEW ToD NANO SECOND */
#define AIROHA_GPON_CURRENT_TOD_SECOND_LOW_32_BITS           0x00dc /* CURRENT ToD SECOND LOW 32 BITS */
#define AIROHA_GPON_CURRENT_TOD_NANO_SECOND                  0x00e0 /* CURRENT ToD NANO SECOND */
#define AIROHA_GPON_TOD_CLOCK_PERIOD                         0x00e4 /* TOD CLOCK PERIOD */
#define AIROHA_GPON_TX_FCS_TABLE_INITIALIZATION_CONTROL      0x0100 /* TX FCS TABLE INITIALIZATION CONTROL */
#define AIROHA_GPON_MIB_CONTROL_STATUS                       0x0120 /* MIB CONTROL AND STATUS */
#define AIROHA_GPON_LOW_32_BITS_MIB_READ_DATA                0x0124 /* LOW 32 BITS of MIB READ DATA */
#define AIROHA_GPON_HIGH_32_BITS_MIB_READ_DATA               0x0128 /* HIGH 32 BITS of MIB READ DATA */
#define AIROHA_GPON_LOW_32_BITS_MIB_WRITE_DATA               0x012c /* LOW 32 BITS of MIB WRITE DATA */
#define AIROHA_GPON_HIGH_32_BITS_MIB_WRITE_DATA              0x0130 /* HIGH 32 BITS of MIB WRITE DATA */
#define AIROHA_GPON_MIB_TABLE_INITIALIZATION_CONTROL         0x0134 /* MIB TABLE INITIALIZATION CONTROL */
#define AIROHA_GPON_GEM_PORT_INDEX_TABLE_ACCESS_CONTROL_MIB  0x0140 /* GEM PORT INDEX TABLE ACCESS CONTROL FOR MIB */
#define AIROHA_GPON_GEM_PORT_INDEX_TABLE_ACCESS_STATUS       0x0144 /* GEM PORT INDEX TABLE ACCESS STATUS */
#define AIROHA_GPON_GEM_PORT_INDEX_TABLE_INITIALIZATION_CONTROL 0x0148 /* GEM PORT INDEX TABLE INITIALIZATION CONTROL */
/* SCU（系统控制单元）复位控制寄存器 1，走 regmap 而非 gpon_base。
 * 偏移与 PON MAC 复位位来自上游 airoha-xpon-epon.c:153-154。
 */
/* 超帧计数器（原厂 gponDevGetSuperframe @0x1ea34，偏移 0x4358）
 * 低 30 位是计数值，高 2 位是内部状态标记
 */
#define AIROHA_GPON_SUPERFRAME_COUNTER                       0x4358
#define AIROHA_GPON_SUPERFRAME_MASK                          0x3fffffff

/* DBA 微调修正值寄存器（原厂 0x4350 / 0x4354，减 0x4000 后）
 * 0x0350: [15:0]=GREEN 修正, [31:16]=YELLOW 修正
 * 0x0354: TOTAL 修正
 */
/* TypeB 保护倒换的队列与通道映射寄存器（原厂 0x4094–0x40b8）
 * 原厂 gpon_dev_typeB_restore 操作的 9 个寄存器
 */
#define AIROHA_GPON_QUEUE_MAP_BASE                           0x0094
#define AIROHA_GPON_QUEUE_ENABLE                             0x00ac
#define AIROHA_GPON_QUEUE_THRESHOLD                          0x00b8
#define AIROHA_GPON_QUEUE_CTRL                               0x0048

#define AIROHA_GPON_DBA_SLIGHT_MODIFY_0                      0x0350
#define AIROHA_GPON_DBA_SLIGHT_MODIFY_1                      0x0354

#define AIROHA_GPON_SCU_RSTCTRL1                              0x834
#define AIROHA_GPON_SCU_PON_MAC_RESET                         BIT(31)

#define AIROHA_GPON_MAC_BM_INTERFACE_STOP                    0x0160 /* MAC AND BM INTERFACE STOP */
#define AIROHA_GPON_FLEXIBLE_ONU_CAPABILITY_SETTING          0x0200 /* FLEXIBLE ONU CAPABILITY SETTING */
#define AIROHA_GPON_UPSTREAM_PREFETCH_DATA_OFFSET            0x0204 /* UPSTREAM PREFETCH DATA OFFSET */
#define AIROHA_GPON_INTERNAL_DELAY                           0x0208 /* INTERNAL DELAY */
#define AIROHA_GPON_THRESHOLD_INSERT_IDLE_GEM                0x020c /* THRESHOLD of INSERT IDLE GEM */
#define AIROHA_GPON_UPSTREAM_NO_MESSAGE_0                    0x0210 /* UPSTREAM NO MESSAGE 0 */
#define AIROHA_GPON_UPSTREAM_NO_MESSAGE_1                    0x0214 /* UPSTREAM NO MESSAGE 1 */
#define AIROHA_GPON_UPSTREAM_NO_MESSAGE_2                    0x0218 /* UPSTREAM NO MESSAGE 2 */
#define AIROHA_GPON_UPSTREAM_DYING_GASP_CONTROL              0x021c /* UPSTREAM DYING GASP CONTROL */
#define AIROHA_GPON_BW_MAP_FITLER_CONTROL                    0x0220 /* BW MAP FITLER CONTROL */
#define AIROHA_GPON_BW_MAP_SFIFO_STATUS                      0x0224 /* BW MAP SFIFO STATUS */
#define AIROHA_GPON_DEBUG_SIGNAL_GROUP_0                     0x0228 /* DEBUG SIGNAL GROUP 0 */
#define AIROHA_GPON_DEBUG_SIGNAL_GROUP_1                     0x022c /* DEBUG SIGNAL GROUP 1 */
#define AIROHA_GPON_MAXIMUM_USED_ENTRIES_TX_FIFO             0x0230 /* THE MAXIMUM USED ENTRIES OF TX FIFO */
#define AIROHA_GPON_RECEIVED_BWM_GRANT_STATUS                0x0234 /* THE RECEIVED BWM GRANT STATUS */
#define AIROHA_GPON_BW_MAP_BFIFO_STATUS                      0x0250 /* BW MAP BFIFO STATUS */
#define AIROHA_GPON_ERROR_CONTROL                            0x0260 /* ERROR CONTROL */
#define AIROHA_GPON_SOFTWARE_RDI_CONTROL                     0x0264 /* SOFTWARE RDI CONTROL */
#define AIROHA_GPON_RX_GEM_COUNTER                           0x0300 /* RX GEM COUNTER */
#define AIROHA_GPON_RX_ETHERNET_FRAME_WITH_ERROR_CRC_COUNTER 0x0304 /* RX ETHERNET FRAME WITH ERROR CRC COUNTER */
#define AIROHA_GPON_RX_GTC_COUNTER                           0x0308 /* RX GTC COUNTER */
#define AIROHA_GPON_TX_GEM_COUNTER                           0x030c /* TX GEM COUNTER */
#define AIROHA_GPON_TX_BST_COUNTER                           0x0310 /* TX BST COUNTER */
#define AIROHA_GPON_RX_GEM_HEADER_ONE_BIT_ERROR_COUNTER      0x0330 /* RX GEM HEADER ONE BIT ERROR COUNTER */
#define AIROHA_GPON_RX_GEM_HEADER_TWO_BIT_ERROR_COUNTER      0x0334 /* RX GEM HEADER TWO BIT ERROR COUNTER */
#define AIROHA_GPON_RX_GEM_HEADER_UNCORRECTABLE_ERROR_COUNTER 0x0338 /* RX GEM HEADER UNCORRECTABLE ERROR COUNTER */
#define AIROHA_GPON_DBA_BACK_DOOR_SW                         0x0340 /* DBA_BACK_DOOR_FOR_SW */
#define AIROHA_GPON_DBA_TOTAL_BUF_SIZE                       0x0344 /* DBA_TOTAL_BUF_SIZE */
#define AIROHA_GPON_DBA_GREEN_SIZE                           0x0348 /* DBA_GREEN_SIZE */
#define AIROHA_GPON_DBA_YELLOW_SIZE                          0x034c /* DBA_YELLOW_SIZE */
#define AIROHA_GPON_DBG_SLIGHT_MODIFY_SIZE_1                 0x0350 /* DBG_SLIGHT_MODIFY_SIZE_1 */
#define AIROHA_GPON_DBG_SLIGHT_MODIFY_SIZE_2                 0x0354 /* DBG_SLIGHT_MODIFY_SIZE_2 */
#define AIROHA_GPON_DS_SPF_CNT                               0x0358 /* DS_SPF_CNT */
#define AIROHA_GPON_TX_SYNC_OFFSET                           0x035c /* TX_SYNC_OFFSET */
#define AIROHA_GPON_PLOAMD_FILTER_O5                         0x0360 /* PLOAMD_FILTER_IN_O5 */
#define AIROHA_GPON_SW_RESYNC                                0x0364 /* SW_RESYNC */
#define AIROHA_GPON_GTC_ETH_EXTR                             0x0368 /* GTC_ETH_EXTR */
#define AIROHA_GPON_DS_GTC_EXTR_ETH_HDR                      0x036c /* DS_GTC_EXTR_ETH_HDR */
#define AIROHA_GPON_US_GTC_EXTR_ETH_HDR                      0x0370 /* US_GTC_EXTR_ETH_HDR */
#define AIROHA_GPON_DS_GTC_EXTR_ETH_CNT                      0x0374 /* DS_GTC_EXTR_ETH_CNT */
#define AIROHA_GPON_US_GTC_EXTR_ETH_CNT                      0x0378 /* US_GTC_EXTR_ETH_CNT */
#define AIROHA_GPON_SNIFF_GTC_INVLD_GEM_BYTE                 0x037c /* SNIFF_GTC_INVLD_GEM_BYTE */
#define AIROHA_GPON_SNIFF_DBG_REG                            0x0380 /* SNIFF_DBG_REG */
#define AIROHA_GPON_SNIFF_US_INFO_FIFO                       0x0384 /* SNIFF_US_INFO_FIFO */
#define AIROHA_GPON_SNIFF_MODE_UP_STREAM_HEADER_TRANSMIT_TIME_WINDOW 0x0388 /* Sniff mode Up stream header transmit time window of stop time */
#define AIROHA_GPON_SNIFF_TX_DA_SA_ADDERESS                  0x038c /* Sniff TX DA SA adderess */
#define AIROHA_GPON_SNIFF_RX_DA_SA_ADDERESS                  0x0390 /* Sniff RX DA SA adderess */
#define AIROHA_GPON_SNIFF_TX_RX_PID                          0x0394 /* Sniff TX RX PID */
#define AIROHA_GPON_SNIFF_MODE_GEM_PORT_COUNTER_ENABLE       0x0398 /* Sniff mode GEM port counter enable */
#define AIROHA_GPON_SNIFF_RX_TX_TPID                         0x039c /* Sniff RX TX TPID */
#define AIROHA_GPON_GPON_MAC_SET                             0x03a0 /* GPON_MAC_SET */
#define AIROHA_GPON_POWER_MANAGEMENT_SLEEP_MODE_CONFIGURATION 0x03a4 /* power management sleep mode configuration */
#define AIROHA_GPON_POWER_MANAGEMENT_SLEEP_COUNTER           0x03a8 /* Power management sleep counter */
#define AIROHA_GPON_TX_SYNC_OPTION                           0x03c0 /* Tx sync option */
#define AIROHA_GPON_O3_O4_US_PLOAM_CONTROL                   0x03c4 /* O3 and O4 US PLOAM CONTROL */
#define AIROHA_GPON_TOD_1PPS_PLUSE_WIDTH_CONTROL             0x03c8 /* TOD 1PPS PLUSE WIDTH CONTROL */

#endif /* _AIROHA_XPON_GPON_REGS_H_ */
