/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2015 - 2023 Beijing WangXun Technology Co., Ltd. */

#ifndef _TXGBE_ETHTOOL_H_
#define _TXGBE_ETHTOOL_H_

int txgbe_get_link_ksettings(struct net_device *netdev,
			     struct ethtool_link_ksettings *cmd);
void txgbe_set_ethtool_ops(struct net_device *netdev);

#endif /* _TXGBE_ETHTOOL_H_ */
