/*
 * Copyright (c) 2005 Ammasso, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <byteswap.h>
#include <semaphore.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <inttypes.h>

#include <rdma/rdma_cma.h>

static int debug = 0;
#define DEBUG_LOG if (debug) printf

/*
 * rping "ping/pong" loop:
 * 	client sends source rkey/addr/len
 *	server receives source rkey/add/len
 *	server rdma reads "ping" data from source
 * 	server sends "go ahead" on rdma read completion
 *	client sends sink rkey/addr/len
 * 	server receives sink rkey/addr/len
 * 	server rdma writes "pong" data to sink
 * 	server sends "go ahead" on rdma write completion
 * 	<repeat loop>
 */

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV, 
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,
	RDMA_READ_ADV,
	RDMA_READ_COMPLETE,
	RDMA_WRITE_ADV,
	RDMA_WRITE_COMPLETE,
	ERROR
};

struct rping_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

/*
 * Default max buffer size for IO...
 */
#define RPING_BUFSIZE 64*1024
#define RPING_SQ_DEPTH 16

/*
 * Control block struct.
 */
struct rping_cb {
	int server;			/* 0 iff client */
	pthread_t cqthread;
	struct ibv_comp_channel *channel;
	struct ibv_cq *cq;
	struct ibv_pd *pd;
	struct ibv_qp *qp;

	struct ibv_recv_wr rq_wr;	/* recv work request record */
	struct ibv_sge recv_sgl;	/* recv single SGE */
	struct rping_rdma_info recv_buf;/* malloc'd buffer */
	struct ibv_mr *recv_mr;		/* MR associated with this buffer */

	struct ibv_send_wr sq_wr;	/* send work requrest record */
	struct ibv_sge send_sgl;
	struct rping_rdma_info send_buf;/* single send buf */
	struct ibv_mr *send_mr;

	struct ibv_send_wr rdma_sq_wr;	/* rdma work request record */
	struct ibv_sge rdma_sgl;	/* rdma single SGE */
	char *rdma_buf;			/* used as rdma sink */
	struct ibv_mr *rdma_mr;

	uint32_t remote_rkey;		/* remote guys RKEY */
	uint64_t remote_addr;		/* remote guys TO */
	uint32_t remote_len;		/* remote guys LEN */

	char *start_buf;		/* rdma read src */
	struct ibv_mr *start_mr;

	enum test_state state;		/* used for cond/signalling */
	sem_t sem;

	uint16_t port;			/* dst port in NBO */
	uint32_t addr;			/* dst addr in NBO */
	char *addr_str;			/* dst addr string */
	int verbose;			/* verbose logging */
	int count;			/* ping count */
	int size;			/* ping data size */
	int validate;			/* validate ping data */

	/* CM stuff */
	pthread_t cmthread;
	struct rdma_cm_id *cm_id;	/* connection on client side,*/
					/* listener on service side. */
	struct rdma_cm_id *child_cm_id;	/* connection on server side */
};

static void rping_cma_event_handler(struct rdma_cm_id *cma_id,
				    struct rdma_cm_event *event)
{
	int ret;
	struct rping_cb *cb = cma_id->context;

	DEBUG_LOG("cma_event type %d cma_id %p (%s)\n", event->event, cma_id,
		  (cma_id == cb->cm_id) ? "parent" : "child");

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		cb->state = ADDR_RESOLVED;
		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			fprintf(stderr, "rdma_resolve_route error %d\n", ret);
			sem_post(&cb->sem);
		}
		break;

	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		cb->state = ROUTE_RESOLVED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_CONNECT_REQUEST:
		cb->state = CONNECT_REQUEST;
		cb->child_cm_id = cma_id;
		DEBUG_LOG("child cma %p\n", cb->child_cm_id);
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ESTABLISHED:
		DEBUG_LOG("ESTABLISHED\n");
		cb->state = CONNECTED;
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		fprintf(stderr, "cma event %d, error %d\n", event->event,
		       event->status);
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DISCONNECTED:
		fprintf(stderr, "DISCONNECT EVENT...\n");
		sem_post(&cb->sem);
		break;

	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		fprintf(stderr, "cma detected device removal!!!!\n");
		break;

	default:
		fprintf(stderr, "oof bad type!\n");
		sem_post(&cb->sem);
		break;
	}
}

static int server_recv(struct rping_cb *cb, struct ibv_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	cb->remote_rkey = cb->recv_buf.rkey;
	cb->remote_addr = cb->recv_buf.buf;
	cb->remote_len  = cb->recv_buf.size;
	DEBUG_LOG("Received rkey %x addr %" PRIx64 "len %d from peer\n",
		  cb->remote_rkey, cb->remote_addr, cb->remote_len);

	if (cb->state == CONNECTED || cb->state == RDMA_WRITE_COMPLETE)
		cb->state = RDMA_READ_ADV;
	else
		cb->state = RDMA_WRITE_ADV;

	return 0;
}

static int client_recv(struct rping_cb *cb, struct ibv_wc *wc)
{
	if (wc->byte_len != sizeof(cb->recv_buf)) {
		fprintf(stderr, "Received bogus data, size %d\n", wc->byte_len);
		return -1;
	}

	if (cb->state == RDMA_READ_ADV)
		cb->state = RDMA_WRITE_ADV;
	else
		cb->state = RDMA_WRITE_COMPLETE;

	return 0;
}

static void rping_cq_event_handler(struct rping_cb *cb)
{
	struct ibv_wc wc;
	struct ibv_recv_wr *bad_wr;
	int ret;

	while ((ret = ibv_poll_cq(cb->cq, 1, &wc)) == 1) {
		if (wc.status) {
			fprintf(stderr, "cq completion failed status %d\n",
				wc.status);
			goto error;
		}

		switch (wc.opcode) {
		case IBV_WC_SEND:
			DEBUG_LOG("send completion\n");
			break;

		case IBV_WC_RDMA_WRITE:
			DEBUG_LOG("rdma write completion\n");
			cb->state = RDMA_WRITE_COMPLETE;
			sem_post(&cb->sem);
			break;

		case IBV_WC_RDMA_READ:
			DEBUG_LOG("rdma read completion\n");
			cb->state = RDMA_READ_COMPLETE;
			sem_post(&cb->sem);
			break;

		case IBV_WC_RECV:
			DEBUG_LOG("recv completion\n");
			ret = cb->server ? server_recv(cb, &wc) :
					   client_recv(cb, &wc);
			if (ret) {
				fprintf(stderr, "recv wc error: %d\n", ret);
				goto error;
			}

			ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
			if (ret) {
				fprintf(stderr, "post recv error: %d\n", ret);
				goto error;
			}
			sem_post(&cb->sem);
			break;

		default:
			DEBUG_LOG("unknown!!!!! completion\n");
			goto error;
		}
	}
	if (ret) {
		fprintf(stderr, "poll error %d\n", ret);
		goto error;
	}
	return;

error:
	cb->state = ERROR;
	sem_post(&cb->sem);
}

static int rping_accept(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	DEBUG_LOG("accepting client connection request\n");

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;

	ret = rdma_accept(cb->child_cm_id, &conn_param);
	if (ret) {
		fprintf(stderr, "rdma_accept error: %d\n", ret);
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state == ERROR) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}
	return 0;
}

static void rping_setup_wr(struct rping_cb *cb)
{
	cb->recv_sgl.addr = (uint64_t) (unsigned long) &cb->recv_buf;
	cb->recv_sgl.length = sizeof cb->recv_buf;
	cb->recv_sgl.lkey = cb->recv_mr->lkey;
	cb->rq_wr.sg_list = &cb->recv_sgl;
	cb->rq_wr.num_sge = 1;

	cb->send_sgl.addr = (uint64_t) (unsigned long) &cb->send_buf;
	cb->send_sgl.length = sizeof cb->send_buf;
	cb->send_sgl.lkey = cb->send_mr->lkey;

	cb->sq_wr.opcode = IBV_WR_SEND;
	cb->sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->sq_wr.sg_list = &cb->send_sgl;
	cb->sq_wr.num_sge = 1;

	cb->rdma_sgl.addr = (uint64_t) (unsigned long) cb->rdma_buf;
	cb->rdma_sgl.lkey = cb->rdma_mr->lkey;
	cb->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
	cb->rdma_sq_wr.sg_list = &cb->rdma_sgl;
	cb->rdma_sq_wr.num_sge = 1;
}

static int rping_setup_buffers(struct rping_cb *cb)
{
	int ret;

	DEBUG_LOG("rping_setup_buffers called on cb %p\n", cb);

	cb->recv_mr = ibv_reg_mr(cb->pd, &cb->recv_buf, sizeof cb->recv_buf,
				 IBV_ACCESS_LOCAL_WRITE);
	if (!cb->recv_mr) {
		fprintf(stderr, "recv_buf reg_mr failed\n");
		return errno;
	}

	cb->send_mr = ibv_reg_mr(cb->pd, &cb->send_buf, sizeof cb->send_buf, 0);
	if (!cb->send_mr) {
		fprintf(stderr, "send_buf reg_mr failed\n");
		ret = errno;
		goto err1;
	}

	cb->rdma_buf = malloc(cb->size);
	if (!cb->rdma_buf) {
		fprintf(stderr, "rdma_buf malloc failed\n");
		ret = -ENOMEM;
		goto err2;
	}

	cb->rdma_mr = ibv_reg_mr(cb->pd, cb->rdma_buf, cb->size,
				 IBV_ACCESS_LOCAL_WRITE |
				 IBV_ACCESS_REMOTE_READ |
				 IBV_ACCESS_REMOTE_WRITE);
	if (!cb->rdma_mr) {
		fprintf(stderr, "rdma_buf reg_mr failed\n");
		ret = errno;
		goto err3;
	}

	if (!cb->server) {
		cb->start_buf = malloc(cb->size);
		if (!cb->start_buf) {
			fprintf(stderr, "start_buf malloc failed\n");
			ret = -ENOMEM;
			goto err4;
		}

		cb->start_mr = ibv_reg_mr(cb->pd, cb->start_buf, cb->size,
					  IBV_ACCESS_LOCAL_WRITE | 
					  IBV_ACCESS_REMOTE_READ |
					  IBV_ACCESS_REMOTE_WRITE);
		if (!cb->start_mr) {
			fprintf(stderr, "start_buf reg_mr failed\n");
			ret = errno;
			goto err5;
		}
	}

	rping_setup_wr(cb);
	DEBUG_LOG("allocated & registered buffers...\n");
	return 0;

err5:
	free(cb->start_buf);
err4:
	ibv_dereg_mr(cb->rdma_mr);
err3:
	free(cb->rdma_buf);
err2:
	ibv_dereg_mr(cb->send_mr);
err1:
	ibv_dereg_mr(cb->recv_mr);
	return ret;
}

static void rping_free_buffers(struct rping_cb *cb)
{
	DEBUG_LOG("rping_free_buffers called on cb %p\n", cb);
	ibv_dereg_mr(cb->recv_mr);
	ibv_dereg_mr(cb->send_mr);
	ibv_dereg_mr(cb->rdma_mr);
	free(cb->rdma_buf);
	if (!cb->server) {
		ibv_dereg_mr(cb->start_mr);
		free(cb->start_buf);
	}
}

static int rping_create_qp(struct rping_cb *cb)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = RPING_SQ_DEPTH;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = cb->cq;
	init_attr.recv_cq = cb->cq;

	if (cb->server) {
		ret = rdma_create_qp(cb->child_cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->child_cm_id->qp;
	} else {
		ret = rdma_create_qp(cb->cm_id, cb->pd, &init_attr);
		if (!ret)
			cb->qp = cb->cm_id->qp;
	}

	return ret;
}

static void rping_free_qp(struct rping_cb *cb)
{
	ibv_destroy_qp(cb->qp);
	ibv_destroy_cq(cb->cq);
	ibv_destroy_comp_channel(cb->channel);
	ibv_dealloc_pd(cb->pd);
}

static int rping_setup_qp(struct rping_cb *cb, struct rdma_cm_id *cm_id)
{
	int ret;

	cb->pd = ibv_alloc_pd(cm_id->verbs);
	if (!cb->pd) {
		fprintf(stderr, "ibv_alloc_pd failed\n");
		return errno;
	}
	DEBUG_LOG("created pd %p\n", cb->pd);

	cb->channel = ibv_create_comp_channel(cm_id->verbs);
	if (!cb->channel) {
		fprintf(stderr, "ibv_create_comp_channel failed\n");
		ret = errno;
		goto err1;
	}
	DEBUG_LOG("created channel %p\n", cb->channel);

	cb->cq = ibv_create_cq(cm_id->verbs, RPING_SQ_DEPTH * 2, cb,
				cb->channel, 0);
	if (!cb->cq) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err2;
	}
	DEBUG_LOG("created cq %p\n", cb->cq);

	ret = ibv_req_notify_cq(cb->cq, 0);
	if (ret) {
		fprintf(stderr, "ibv_create_cq failed\n");
		ret = errno;
		goto err3;
	}

	ret = rping_create_qp(cb);
	if (ret) {
		fprintf(stderr, "rping_create_qp failed: %d\n", ret);
		goto err3;
	}
	DEBUG_LOG("created qp %p\n", cb->qp);
	return 0;

err3:
	ibv_destroy_cq(cb->cq);
err2:
	ibv_destroy_comp_channel(cb->channel);
err1:
	ibv_dealloc_pd(cb->pd);
	return ret;
}

static void *cm_thread(void *arg)
{
	struct rdma_cm_event *event;
	int ret;

	while (1) {
		ret = rdma_get_cm_event(&event);
		if (ret) {
			fprintf(stderr, "rdma_get_cm_event err %d\n", ret);
			exit(ret);
		}
		rping_cma_event_handler(event->id, event);
		rdma_ack_cm_event(event);
	}
}

static void *cq_thread(void *arg)
{
	struct rping_cb *cb = arg;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	int ret;
	
	DEBUG_LOG("cq_thread started.\n");

	while (1) {	
		ret = ibv_get_cq_event(cb->channel, &ev_cq, &ev_ctx);
		if (ret) {
			fprintf(stderr, "Failed to get cq event!\n");
			exit(ret);
		}
		if (ev_cq != cb->cq) {
			fprintf(stderr, "Unkown CQ!\n");
			exit(-1);
		}
		ret = ibv_req_notify_cq(cb->cq, 0);
		if (ret) {
			fprintf(stderr, "Failed to set notify!\n");
			exit(ret);
		}
		rping_cq_event_handler(cb);
		ibv_ack_cq_events(cb->cq, 1);
	}
}

static void rping_format_send(struct rping_cb *cb, char *buf, struct ibv_mr *mr)
{
	struct rping_rdma_info *info = &cb->send_buf;

	info->buf = (uint64_t) (unsigned long) buf;
	info->rkey = mr->rkey;
	info->size = cb->size;

	DEBUG_LOG("RDMA addr %" PRIx64" rkey %x len %d\n",
		  info->buf, info->rkey, info->size);
}

static void rping_test_server(struct rping_cb *cb)
{
	struct ibv_send_wr *bad_wr;
	int ret;

	while (1) {
		/* Wait for client's Start STAG/TO/Len */
		sem_wait(&cb->sem);
		if (cb->state != RDMA_READ_ADV) {
			fprintf(stderr, "wait for RDMA_READ_ADV state %d\n",
				cb->state);
			break;
		}

		DEBUG_LOG("server received sink adv\n");

		/* Issue RDMA Read. */
		cb->rdma_sq_wr.opcode = IBV_WR_RDMA_READ;
		cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.sg_list->length = cb->remote_len;

		ret = ibv_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			fprintf(stderr, "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted rdma read req \n");

		/* Wait for read completion */
		sem_wait(&cb->sem);
		if (cb->state != RDMA_READ_COMPLETE) {
			fprintf(stderr, "wait for RDMA_READ_COMPLETE state %d\n",
				cb->state);
			break;
		}
		DEBUG_LOG("server received read complete\n");

		/* Display data in recv buf */
		if (cb->verbose)
			printf("server ping data: %s\n", cb->rdma_buf);

		/* Tell client to continue */
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			fprintf(stderr, "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");

		/* Wait for client's RDMA STAG/TO/Len */
		sem_wait(&cb->sem);
		if (cb->state != RDMA_WRITE_ADV) {
			fprintf(stderr, "wait for RDMA_WRITE_ADV state %d\n",
				cb->state);
			break;
		}
		DEBUG_LOG("server received sink adv\n");

		/* RDMA Write echo data */
		cb->rdma_sq_wr.opcode = IBV_WR_RDMA_WRITE;
		cb->rdma_sq_wr.wr.rdma.rkey = cb->remote_rkey;
		cb->rdma_sq_wr.wr.rdma.remote_addr = cb->remote_addr;
		cb->rdma_sq_wr.sg_list->length = strlen(cb->rdma_buf) + 1;
		DEBUG_LOG("rdma write from lkey %x laddr %" PRIx64 " len %d\n",
			  cb->rdma_sq_wr.sg_list->lkey,
			  cb->rdma_sq_wr.sg_list->addr,
			  cb->rdma_sq_wr.sg_list->length);

		ret = ibv_post_send(cb->qp, &cb->rdma_sq_wr, &bad_wr);
		if (ret) {
			fprintf(stderr, "post send error %d\n", ret);
			break;
		}

		/* Wait for completion */
		ret = sem_wait(&cb->sem);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			fprintf(stderr, "wait for RDMA_WRITE_COMPLETE state %d\n",
				cb->state);
			break;
		}
		DEBUG_LOG("server rdma write complete \n");

		/* Tell client to begin again */
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			fprintf(stderr, "post send error %d\n", ret);
			break;
		}
		DEBUG_LOG("server posted go ahead\n");
	}
}

static int rping_bind_server(struct rping_cb *cb)
{
	struct sockaddr_in sin;
	int ret;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = cb->addr;
	sin.sin_port = cb->port;

	ret = rdma_bind_addr(cb->cm_id, (struct sockaddr *) &sin);
	if (ret) {
		fprintf(stderr, "rdma_bind_addr error %d\n", ret);
		return ret;
	}
	DEBUG_LOG("rdma_bind_addr successful\n");

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(cb->cm_id, 3);
	if (ret) {
		fprintf(stderr, "rdma_listen failed: %d\n", ret);
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != CONNECT_REQUEST) {
		fprintf(stderr, "wait for CONNECT_REQUEST state %d\n",
			cb->state);
		return -1;
	}

	return 0;
}

static void rping_run_server(struct rping_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int ret;

	ret = rping_bind_server(cb);
	if (ret)
		return;

	ret = rping_setup_qp(cb, cb->child_cm_id);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		return;
	}

	ret = rping_setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "rping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
		goto err2;
	}

	pthread_create(&cb->cqthread, NULL, cq_thread, cb);

	ret = rping_accept(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		goto err2;
	}

	rping_test_server(cb);
	rdma_disconnect(cb->child_cm_id);
	rdma_destroy_id(cb->child_cm_id);
err2:
	rping_free_buffers(cb);
err1:
	rping_free_qp(cb);
}

static void rping_test_client(struct rping_cb *cb)
{
	int ping, start, cc, i, ret;
	struct ibv_send_wr *bad_wr;
	unsigned char c;

	start = 65;
	for (ping = 0; !cb->count || ping < cb->count; ping++) {
		cb->state = RDMA_READ_ADV;

		/* Put some ascii text in the buffer. */
		cc = sprintf(cb->start_buf, "rdma-ping-%d: ", ping);
		for (i = cc, c = start; i < cb->size; i++) {
			cb->start_buf[i] = c;
			c++;
			if (c > 122)
				c = 65;
		}
		start++;
		if (start > 122)
			start = 65;
		cb->start_buf[cb->size - 1] = 0;

		rping_format_send(cb, cb->start_buf, cb->start_mr);
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			fprintf(stderr, "post send error %d\n", ret);
			break;
		}

		/* Wait for server to ACK */
		sem_wait(&cb->sem);
		if (cb->state != RDMA_WRITE_ADV) {
			fprintf(stderr, "wait for RDMA_WRITE_ADV state %d\n",
				cb->state);
			break;
		}

		rping_format_send(cb, cb->rdma_buf, cb->rdma_mr);
		ret = ibv_post_send(cb->qp, &cb->sq_wr, &bad_wr);
		if (ret) {
			fprintf(stderr, "post send error %d\n", ret);
			break;
		}

		/* Wait for the server to say the RDMA Write is complete. */
		sem_wait(&cb->sem);
		if (cb->state != RDMA_WRITE_COMPLETE) {
			fprintf(stderr, "wait for RDMA_WRITE_COMPLETE state %d\n",
				cb->state);
			break;
		}

		if (cb->validate)
			if (memcmp(cb->start_buf, cb->rdma_buf, cb->size)) {
				fprintf(stderr, "data mismatch!\n");
				break;
			}

		if (cb->verbose)
			printf("ping data: %s\n", cb->rdma_buf);
	}
}

static int rping_connect_client(struct rping_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		fprintf(stderr, "rdma_connect error %d\n", ret);
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != CONNECTED) {
		fprintf(stderr, "wait for CONNECTED state %d\n", cb->state);
		return -1;
	}

	DEBUG_LOG("rmda_connect successful\n");
	return 0;
}

static int rping_bind_client(struct rping_cb *cb)
{
	struct sockaddr_in sin;
	int ret;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = cb->addr;
	sin.sin_port = cb->port;

	ret = rdma_resolve_addr(cb->cm_id, NULL, (struct sockaddr *) &sin,
				2000);
	if (ret) {
		fprintf(stderr, "rdma_resolve_addr error %d\n", ret);
		return ret;
	}

	sem_wait(&cb->sem);
	if (cb->state != ROUTE_RESOLVED) {
		fprintf(stderr, "waiting for addr/route resolution state %d\n",
			cb->state);
		return ret;
	}

	DEBUG_LOG("rdma_resolve_addr - rdma_resolve_route successful\n");
	return 0;
}

static void rping_run_client(struct rping_cb *cb)
{
	struct ibv_recv_wr *bad_wr;
	int ret;

	ret = rping_bind_client(cb);
	if (ret)
		return;

	ret = rping_setup_qp(cb, cb->cm_id);
	if (ret) {
		fprintf(stderr, "setup_qp failed: %d\n", ret);
		return;
	}

	ret = rping_setup_buffers(cb);
	if (ret) {
		fprintf(stderr, "rping_setup_buffers failed: %d\n", ret);
		goto err1;
	}

	ret = ibv_post_recv(cb->qp, &cb->rq_wr, &bad_wr);
	if (ret) {
		fprintf(stderr, "ibv_post_recv failed: %d\n", ret);
		goto err2;
	}

	pthread_create(&cb->cqthread, NULL, cq_thread, cb);

	ret = rping_connect_client(cb);
	if (ret) {
		fprintf(stderr, "connect error %d\n", ret);
		goto err2;
	}

	rping_test_client(cb);
	rdma_disconnect(cb->cm_id);
err2:
	rping_free_buffers(cb);
err1:
	rping_free_qp(cb);
}

static void usage(char *name)
{
	printf("%s -c|s [-vVd] [-S size] [-C count] -a addr -p port\n", 
	       basename(name));
	printf("\t-c\t\tclient side\n");
	printf("\t-s\t\tserver side\n");
	printf("\t-v\t\tdisplay ping data to stdout\n");
	printf("\t-V\t\tverbosity\n");
	printf("\t-d\t\tdebug printfs\n");
	printf("\t-S size \tping data size\n");
	printf("\t-C count\tping count times\n");
	printf("\t-a addr\t\taddress\n");
	printf("\t-p port\t\tport\n");
}

int main(int argc, char *argv[])
{
	struct rping_cb *cb;
	int op;
	int ret = 0;

	cb = malloc(sizeof(*cb));
	if (!cb)
		return -ENOMEM;

	memset(cb, 0, sizeof(*cb));
	cb->server = -1;
	cb->state = IDLE;
	cb->size = 64;
	sem_init(&cb->sem, 0, 0);

	opterr = 0;
	while ((op=getopt(argc, argv, "a:p:C:S:t:scvVd")) != -1) {
		switch (op) {
		case 'a':
			cb->addr_str = optarg;
			cb->addr = inet_addr(optarg);
			DEBUG_LOG("ipaddr (%s)\n", optarg);
			break;
		case 'p':
			cb->port = htons(atoi(optarg));
			DEBUG_LOG("port %d\n", (int) atoi(optarg));
			break;
		case 's':
			cb->server = 1;
			DEBUG_LOG("server\n");
			break;
		case 'c':
			cb->server = 0;
			DEBUG_LOG("client\n");
			break;
		case 'S':
			cb->size = atoi(optarg);
			if ((cb->size < 1) ||
			    (cb->size > (RPING_BUFSIZE - 1))) {
				fprintf(stderr, "Invalid size %d "
				       "(valid range is 1 to %d)\n",
				       cb->size, RPING_BUFSIZE);
				ret = EINVAL;
			} else
				DEBUG_LOG("size %d\n", (int) atoi(optarg));
			break;
		case 'C':
			cb->count = atoi(optarg);
			if (cb->count < 0) {
				fprintf(stderr, "Invalid count %d\n",
					cb->count);
				ret = EINVAL;
			} else
				DEBUG_LOG("count %d\n", (int) cb->count);
			break;
		case 'v':
			cb->verbose++;
			DEBUG_LOG("verbose\n");
			break;
		case 'V':
			cb->validate++;
			DEBUG_LOG("validate data\n");
			break;
		case 'd':
			debug++;
			break;
		default:
			usage("rping");
			ret = EINVAL;
			break;
		}
	}
	if (ret)
		goto out;

	if (cb->server == -1) {
		usage("rping");
		ret = EINVAL;
		goto out;
	}

	ret = rdma_create_id(&cb->cm_id, cb);
	if (ret) {
		ret = errno;
		fprintf(stderr, "rdma_create_id error %d\n", ret);
		goto out;
	}
	DEBUG_LOG("created cm_id %p\n", cb->cm_id);

	pthread_create(&cb->cmthread, NULL, cm_thread, cb);

	if (cb->server)
		rping_run_server(cb);
	else
		rping_run_client(cb);

	DEBUG_LOG("destroy cm_id %p\n", cb->cm_id);
	rdma_destroy_id(cb->cm_id);
out:
	free(cb);
	return ret;
}
