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

#define get_data_ptr(mad) ((void *) ((mad).hdr.data))
static int send_classportinfo(struct cma_ece *ece, uint8_t method)
{
	struct umad_class_port_info *cpi;
	struct ib_user_mad umad = {};
	struct umad_packet *out_mad = (void *)umad.data;
	int ret;

	out_mad->mad_hdr.base_version = UMAD_BASE_VERSION;
	out_mad->mad_hdr.method = method;
	out_mad->mad_hdr.attr_id = UMAD_ATTR_CLASS_PORT_INFO;
	out_mad->mad_hdr.attr_mod = htobe32(1 >> 13);
	out_mad->mad_hdr.mgmt_class = UMAD_CLASS_CM;
	out_mad->mad_hdr.class_version = UMAD_CM_CLASS_VERSION;

	/* TOOD: configure timeout */
	ret = umad_send(ece->portfd, ece->agent, &umad, sizeof(umad), 100, 0);
	printf("%s ret = %d\n", __func__, ret);
	return ret;
}

static int recv_classportinfo(struct cma_ece *ece)
{
	struct umad_class_port_info *cpi;
	struct ib_user_mad umad;
	int length;
	int ret;

	/* TODO: connfigure timeout */
	ret = umad_recv(ece->portfd, &umad, &length, 100);
	printf("%s ret = %d\n", __func__, ret);
	return ret;
}

int ece_request_cap(struct cma_device *dev, uint8_t port_num)
{
	int ret;

	if (!dev->ece)
		return 0;

	printf("%s\n", __func__);

	/* CM GET CAP, CM SET CAP back, send options, recv, options */
	ret = send_classportinfo(&dev->ece[port_num - 1], UMAD_METHOD_GET);
	ret = recv_classportinfo(&dev->ece[port_num - 1]);
	if (ret == -ETIMEDOUT)
		/* Working against old librdmacm without ECE */
		return 0;

	return ret;
}

int ece_reply_cap(struct cma_device *dev, uint8_t port_num)
{
	int ret;

	if (!dev->ece)
		return 0;

	printf("%s\n", __func__);

	ret = recv_classportinfo(&dev->ece[port_num - 1]);
	if (ret == -ETIMEDOUT) {
		/*
		 * We didn't recieve ECE CM_GET, no need to send back
		 * anything. Continue in legacy mode,
		 */
		return 0;
	}
	ret = send_classportinfo(&dev->ece[port_num - 1], UMAD_METHOD_SEND);

	return ret;
}
