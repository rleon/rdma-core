// SPDX-License-Identifier: (GPL-2.0 OR Linux-OpenIB)
/*
 * Copyright (c) 2019, Mellanox Technologies inc.  All rights reserved.
 */

#include "cma.h"
#include <stdio.h>

#include <infiniband/umad.h>
#include <infiniband/umad_types.h>

#define UMAD_CM_CLASS_VERSION 1

int ece_init_cm(struct cma_device *dev)
{
	struct umad_reg_attr reg_attr = { .mgmt_class = UMAD_CLASS_CM,
					  .mgmt_class_version =
						  UMAD_CM_CLASS_VERSION };
	int i, ret = 0;

	/* The caller halds pthread_mutex_lock(&eut) */
	if (dev->ece)
		/* We already initialized CM channel for this device */
		return 0;

	dev->ece = calloc(dev->port_cnt, sizeof(*dev->port));
	if (!dev->ece)
		return ERR(ENOMEM);

	/*
	 * We are placing this chek after calloc() to ensure
	 * that we have visible marker for ece_close_cm().
	 */
	if (umad_init() < 0) {
		/*
		 * libibumad can't be initialized, no big deal,
		 * fallback to old mode, where ECE wasn't supported.
		 */
		goto err_umad_init;
	}


	for (i = 0; i < dev->port_cnt; i++) {
		const char *dev_name = dev->verbs->device->name;

		/*
		 * Access to /dev/infiniband/umad* requires root
		 * permissions, if we don't have, simply fallback
		 * to legacy mode without ECE support.
		 */
		dev->ece[i].portfd = umad_open_port(dev_name, i + 1);
		if (dev->ece[i].portfd < 0) {
			ret = (dev->ece[i].portfd == -EIO) ?
				      0 :
				      ERR(-1 * dev->ece[i].portfd);
			goto err_open_port;
		}

		ret = umad_register2(dev->ece[i].portfd, &reg_attr,
				     &dev->ece[i].agent);
		if (ret) {
			ret = ERR(ret);
			goto err_register;
		}
	}

	return 0;

err_register:
	umad_close_port(dev->ece[i].portfd);

err_open_port:
	while (i--) {
		umad_unregister(dev->ece[i].portfd, dev->ece[i].agent);
		umad_close_port(dev->ece[i].portfd);
	}

	umad_done();

err_umad_init:
	free(dev->ece);
	dev->ece = NULL;

	return ret;
}

void ece_close_cm(struct cma_device *dev)
{
	int i;

	if (!dev->ece)
		return;

	for (i = 0; i < dev->port_cnt; i++) {
		umad_unregister(dev->ece[i].portfd, dev->ece[i].agent);
		umad_close_port(dev->ece[i].portfd);
	}

	umad_done();
	free(dev->ece);
	dev->ece = NULL;
}
