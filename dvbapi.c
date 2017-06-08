/*
   - * Copyright (C) 2014-2020 Catalin Toda <catalinii@yahoo.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 *
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/if.h>
#include <fcntl.h>
#include <ctype.h>
#include "utils.h"
#include "dvb.h"
#include "socketworks.h"
#include "minisatip.h"
#include "dvbapi.h"
#include "adapter.h"
#include "tables.h"
#include "pmt.h"

#define DEFAULT_LOG LOG_DVBAPI

const int64_t DVBAPI_ITEM = 0x1000000000000;
int dvbapi_sock = -1;
int sock;
int dvbapi_is_enabled = 0;
int enabledKeys = 0;
int network_mode = 1;
int dvbapi_protocol_version = DVBAPI_PROTOCOL_VERSION;
int dvbapi_ca = -1;

SKey *keys[MAX_KEYS];
SMutex keys_mutex;
unsigned char read_buffer[1500];

#define TEST_WRITE(a, xlen)                                                                                                                     \
	{                                                                                                                                           \
		int x;                                                                                                                                  \
		mutex_lock(&keys_mutex);                                                                                                                \
		if ((x = (a)) != (xlen))                                                                                                                \
		{                                                                                                                                       \
			LOG("write to dvbapi socket failed (%d out of %d), closing socket %d, errno %d, error: %s", x, xlen, sock, errno, strerror(errno)); \
			sockets_del(dvbapi_sock);                                                                                                           \
			sock = 0;                                                                                                                           \
			dvbapi_sock = -1;                                                                                                                   \
			dvbapi_is_enabled = 0;                                                                                                              \
		}                                                                                                                                       \
		mutex_unlock(&keys_mutex);                                                                                                              \
	}

static inline SKey *get_key(int i)
{
	if (i < 0 || i >= MAX_KEYS || !keys[i] || !keys[i]->enabled)
		return NULL;
	return keys[i];
}

void invalidate_adapter(int aid)
{
	return;
}

int get_index_for_filter(SKey *k, int filter)
{
	int i;
	for (i = 0; i < MAX_KEY_FILTERS; i++)
		if (k->filter_id[i] == filter)
			return i;
	return -1;
}

#define dvbapi_copy32r(v, a, i) \
	if (change_endianness)      \
	copy32rr(v, a, i) else copy32r(v, a, i)
#define dvbapi_copy16r(v, a, i) \
	if (change_endianness)      \
	copy16rr(v, a, i) else copy16r(v, a, i)

int dvbapi_reply(sockets *s)
{
	unsigned char *b = s->buf;
	SKey *k;
	int change_endianness = 0;
	unsigned int op, _pid;
	int k_id, a_id = 0, pos = 0;
	int demux, filter;

	if (s->rlen == 0)
	{
		send_client_info(s);
		return 0;
	}
	while (pos < s->rlen)
	{
		int op1;
		b = s->buf + pos;
		copy32r(op, b, 0);
		op1 = op & 0xFFFFFF;
		change_endianness = 0;
		if (op1 == CA_SET_DESCR_X || op1 == CA_SET_DESCR_AES_X || op1 == CA_SET_PID_X || op1 == DMX_STOP_X || op1 == DMX_SET_FILTER_X || op1 == CA_SET_DESCR_AES_X)
		{ // change endianness
			op = 0x40000000 | ((op1 & 0xFF) << 16) | (op1 & 0xFF00) | ((op1 & 0xFF0000) >> 16);
			if (!(op & 0xFF0000))
				op &= 0xFFFFFF;
			LOG("dvbapi: changing endianness from %06X to %08X", op1, op);
			//b ++;
			//pos ++;
			b[4] = b[0];
			change_endianness = 1;
		}
		LOG("dvbapi read from socket %d the following data (%d bytes), pos = %d, op %08X, key %d",
			s->sock, s->rlen, pos, op, b[4]);
		//		LOGL(3, "dvbapi read from socket %d the following data (%d bytes), pos = %d, op %08X, key %d -> %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X", s->sock, s->rlen, pos, op, b[4], b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10]);

		switch (op)
		{

		case DVBAPI_SERVER_INFO:

			if (s->rlen < 6)
				return 0;
			dvbapi_copy16r(dvbapi_protocol_version, b, 4);
			LOG("dvbapi: server version %d found, name = %s",
				dvbapi_protocol_version, b + 7);
			if (dvbapi_protocol_version > DVBAPI_PROTOCOL_VERSION)
				dvbapi_protocol_version = DVBAPI_PROTOCOL_VERSION;

			register_dvbapi();
			dvbapi_is_enabled = 1;
			pos = 6 + strlen((const char *)b + 6) + 1;
			break;

		case DVBAPI_DMX_SET_FILTER:
		{
			SKey *k;
			int not_found = 1;
			int i;
			if (change_endianness)
				pos += 2; // for some reason the packet is longer with 2 bytes
			pos += 65;
			dvbapi_copy16r(_pid, b, 7);
			_pid &= 0x1FFF;
			k_id = b[4] - opts.dvbapi_offset;
			k = get_key(k_id);
			a_id = -1;
			if (k)
				a_id = k->adapter;
			demux = b[5];
			filter = b[6];
			LOG(
				"dvbapi set filter for pid %04X (%d), key %d, demux %d, filter %d %s",
				_pid, _pid, k_id, demux, filter,
				!k ? "(KEY NOT VALID)" : "");
			LOG("filter: %02X %02X %02X %02X %02X, mask: %02X %02X %02X %02X %02X", b[9], b[10], b[11], b[12], b[13], b[25], b[26], b[27], b[28], b[29]);
			i = -1;
			int fid = -1;
			if (k)
			{
				for (i = 0; i < MAX_KEY_FILTERS; i++)
					if (k->filter_id[i] >= 0 && k->pid[i] == _pid && k->demux[i] == demux && k->filter[i] == filter)
					{
						not_found = 0;
						break;
					}
				if (not_found)
				{

					fid = add_filter_mask(k->adapter, _pid, (void *)send_ecm, (void *)k, FILTER_ADD_REMOVE, b + 9, b + 25);
					i = get_index_for_filter(k, -1);
				}
				else
					LOG("dvbapi: filter for pid %d and key %d already exists", _pid, k->id);
			}
			if (i >= 0 && fid >= 0)
			{
				k->filter_id[i] = fid;
				k->filter[i] = filter;
				k->demux[i] = demux;
				k->pid[i] = _pid;
				k->ecm_parity[i] = -1;
				if (k)
					k->ecms++;
				update_pids(a_id);
			}
			else if (not_found)
				LOG("dvbapi: DMX_SET_FILTER failed, fid %d, index %d", fid, i);
			break;
		}
		case DVBAPI_DMX_STOP:
		{
			int i;
			k_id = b[4] - opts.dvbapi_offset;
			demux = b[5];
			filter = b[6];
			pos += 9;
			k = get_key(k_id);
			if (!k)
				break;
			a_id = k->adapter;
			dvbapi_copy16r(_pid, b, 7)
				_pid &= 0x1FFF;
			for (i = 0; i < MAX_KEY_FILTERS; i++)
				if (k->filter[i] == filter && k->demux[i] == demux && k->pid[i] == _pid)
					break;
			LOG(
				"dvbapi: received DMX_STOP for key %d, index %d, adapter %d, demux %d, filter %d, pid %X (%d)",
				k_id, i, a_id, demux, filter, _pid, _pid);
			if (i < MAX_KEY_FILTERS && i >= 0)
				del_filter(k->filter_id[i]);
			k->filter_id[i] = -1;
			k->pid[i] = -1;

			if (k)
			{
				k->ecms--;
				k->last_dmx_stop = getTick();
				//			if (k->ecms <= 0)
				//				close_pmt_for_ca(dvbapi_ca, get_adapter(k->adapter), get_pmt(k->pmt_id));
			}

			break;
		}
		case DVBAPI_CA_SET_PID:
		{
			LOG("received DVBAPI_CA_SET_PID");
			pos += 13;
			break;
		}
		case DVBAPI_CA_SET_DESCR:
		{
			int index, parity, k_id;
			SKey *k;
			unsigned char *cw;

			pos += 21;
			k_id = b[4] - opts.dvbapi_offset;
			dvbapi_copy32r(index, b, 5);
			dvbapi_copy32r(parity, b, 9);
			cw = b + 13;
			k = get_key(k_id);
			if (k && (parity < 2))
			{
				mutex_lock(&k->mutex);

				k->key_len = 8;
				memcpy(k->cw[parity], cw, k->key_len);

				LOG(
					"dvbapi: received DVBAPI_CA_SET_DESCR, key %d parity %d, index %d, CW: %02X %02X %02X %02X %02X %02X %02X %02X",
					k_id, parity, index, cw[0], cw[1], cw[2], cw[3], cw[4],
					cw[5], cw[6], cw[7]);

				send_cw(k->pmt_id, k->algo, parity, cw, NULL);

				mutex_unlock(&k->mutex);
			}
			else
				LOG(
					"dvbapi: invalid DVBAPI_CA_SET_DESCR, key %d parity %d, k %p, index %d, CW: %02X %02X %02X %02X %02X %02X %02X %02X",
					k_id, parity, k, index, cw[0], cw[1], cw[2], cw[3],
					cw[4], cw[5], cw[6], cw[7]);
			break;
		}

		case DVBAPI_ECM_INFO:
		{
			int pos1 = s->rlen - pos;
			SKey *k = get_key(b[4] - opts.dvbapi_offset);
			unsigned char cardsystem[255];
			unsigned char reader[255];
			unsigned char from[255];
			unsigned char protocol[255];
			unsigned char len = 0;
			unsigned char *msg[5] =
				{cardsystem, reader, from, protocol, NULL};
			int i = 5, j = 0;
			uint16_t sid;

			copy16r(sid, b, i);

			if (k)
			{
				mutex_lock(&k->mutex);
				msg[0] = k->cardsystem;
				msg[1] = k->reader;
				msg[2] = k->from;
				msg[3] = k->protocol;
				copy16r(k->caid, b, i + 2);
				copy16r(k->info_pid, b, i + 4);
				copy32r(k->prid, b, i + 6);
				copy32r(k->ecmtime, b, i + 10);
			}
			i += 14;
			while (msg[j] && i < pos1)
			{
				len = b[i++];
				memset(msg[j], 0, sizeof(k->cardsystem));
				if (len >= sizeof(k->cardsystem) - 2)
					len = sizeof(k->cardsystem) - 2;
				memcpy(msg[j], b + i, len);
				msg[j][len] = 0;
				i += len;
				j++;
			}
			if (i < pos1 && k)
				k->hops = b[i++];
			if (k)
				mutex_unlock(&k->mutex);
			pos += i;
			LOG(
				"dvbapi: ECM_INFO: key %d, SID = %04X, CAID = %04X (%s), PID = %d (%04X), ProvID = %06X, ECM time = %d ms, reader = %s, from = %s, protocol = %s, hops = %d",
				k ? k->id : -1, sid, k ? k->caid : 0, msg[0],
				k ? k->info_pid : 0, k ? k->info_pid : 0, k ? k->prid : 0,
				k ? k->ecmtime : -1, msg[1], msg[2], msg[3],
				k ? k->hops : 0);
			break;
		}

		case CA_SET_DESCR_MODE:
		{
			int k_id, algo, mode;
			SKey *k;
			pos += 17;
			k_id = b[4] - opts.dvbapi_offset;
			dvbapi_copy32r(algo, b, 5);
			dvbapi_copy32r(mode, b, 9);
			LOG("Key %d, Algo set to %d, Mode set to %d", k_id, algo, mode);
			k = get_key(k_id);
			if (!k)
				break;
			set_algo(k, algo, mode);
			break;
		}

		default:
		{
			LOG(
				"dvbapi: unknown operation: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
				b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
				b[10]);
			pos = s->rlen;
		}
		}
	}
	s->rlen = 0;
	return 0;
}

int dvbapi_send_pmt(SKey *k)
{
	unsigned char buf[1500];
	int len;

	LOG(
		"Sending pmt to dvbapi server for pid %d, Channel ID %04X, key %d, using socket %d",
		k->pmt_pid, k->sid, k->id, sock);
	memset(buf, 0, sizeof(buf));
	copy32(buf, 0, AOT_CA_PMT);
	buf[6] = CAPMT_LIST_UPDATE;
	//	buf[6] = CAPMT_LIST_ONLY;
	copy16(buf, 7, k->sid);
	buf[9] = 1;

	copy32(buf, 12, 0x01820200);
	buf[15] = k->id + opts.dvbapi_offset;
	buf[16] = k->id + opts.dvbapi_offset;
	memcpy(buf + 17, k->pi, k->pi_len);
	len = 17 - 6 + k->pi_len + 2;
	copy16(buf, 4, len);
	copy16(buf, 10, len - 11);
	TEST_WRITE(write(sock, buf, len + 6), len + 6);
	return 0;
}

int dvbapi_close(sockets *s)
{
	int i;
	LOG("requested dvbapi close for sock %d, sock_id %d", sock, dvbapi_sock,
		s->sock);
	sock = 0;
	dvbapi_is_enabled = 0;
	SKey *k;
	for (i = 0; i < MAX_KEYS; i++)
		if (keys[i] && keys[i]->enabled)
		{
			k = get_key(i);
			if (!k)
				continue;
			keys_del(i);
		}
	unregister_dvbapi();
	return 0;
}

int dvbapi_timeout(sockets *s)
{
	//	if (!enabledKeys)return 1; // close dvbapi connection
	return 0;
}

int connect_dvbapi(void *arg)
{

	if ((sock > 0) && dvbapi_is_enabled) // already connected
	{
		int i;
		uint64_t ctime = getTick();

		for (i = 0; i < MAX_KEYS; i++)
			if (keys[i] && keys[i]->enabled && (keys[i]->ecms == 0) && (keys[i]->last_dmx_stop > 0) && (ctime - keys[i]->last_dmx_stop > 3000))
			{
				LOG("Key %d active but no active filter, closing ", i);
				keys_del(i);
			}
		return 0;
	}

	dvbapi_is_enabled = 0;

	if (!opts.dvbapi_port || !opts.dvbapi_host)
		return 0;

	if (sock <= 0)
	{
		if (opts.dvbapi_host[0] == '/')
		{
			network_mode = 0;
			sock = connect_local_socket(opts.dvbapi_host, 1);
		}
		else
			sock = tcp_connect(opts.dvbapi_host, opts.dvbapi_port, NULL, 1);
		if (sock < 0)
			LOG_AND_RETURN(0, "%s: connect to %s failed", __FUNCTION__, opts.dvbapi_host);
		dvbapi_sock = sockets_add(sock, NULL, -1, TYPE_TCP | TYPE_CONNECT,
								  (socket_action)dvbapi_reply, (socket_action)dvbapi_close,
								  (socket_action)dvbapi_timeout);
		if (dvbapi_sock < 0)
			LOG_AND_RETURN(0, "%s: socket_add failed", __FUNCTION__);
		set_socket_buffer(dvbapi_sock, read_buffer, sizeof(read_buffer));
		sockets_timeout(dvbapi_sock, 2000); // 2s timeout to close the socket
		return 0;
	}
	return 0;
}

int poller_sock;
void init_dvbapi()
{
	int sec = 1;
	poller_sock = sockets_add(SOCK_TIMEOUT, NULL, -1, TYPE_UDP,
							  NULL, NULL, (socket_action)connect_dvbapi);
	sockets_timeout(poller_sock, sec * 1000); // try to connect every 1s
	set_sockets_rtime(poller_sock, -sec * 1000);
	mutex_init(&keys_mutex);
}

void send_client_info(sockets *s)
{
	char buf[1000];
	unsigned char len;
	memset(buf, 0, sizeof(buf));
	copy32(buf, 0, DVBAPI_CLIENT_INFO);
	copy16(buf, 4, dvbapi_protocol_version)
		len = sprintf(buf + 7, "%s/%s", app_name, version);
	buf[6] = len;
	dvbapi_is_enabled = 1;
	TEST_WRITE(write(s->sock, buf, len + 7), len + 7);
}

int send_ecm(int filter_id, unsigned char *b, int len, void *opaque)
{
	SKey *k = NULL;
	SPMT *pmt;
	uint8_t buf[1600];
	int i, pid;
	int filter, demux;
	int old_parity;

	if (!dvbapi_is_enabled)
		return 0;
	pid = get_filter_pid(filter_id);
	if (pid == -1)
		LOG_AND_RETURN(0, "%s: pid not found in filter", __FUNCTION__, pid);

	k = (void *)opaque;
	if (!k || !k->enabled)
		LOG_AND_RETURN(0, "%s: key is null pid %d and filter %d", __FUNCTION__, pid, filter_id);
	pmt = get_pmt(k->pmt_id);
	if (!pmt)
		LOG_AND_RETURN(0, "%s: PMT not found for pid %d and filter %d", __FUNCTION__, pid, filter_id);

	i = get_index_for_filter(k, filter_id);
	if (i == -1)
		LOG_AND_RETURN(0, "%s: filter %d not found", __FUNCTION__, filter_id);

	demux = k->demux[i];
	filter = k->filter[i];
	//	LOG("%s: pid %d %d %02X", __FUNCTION__, pid, k->ecm_parity[i], b[1]);

	if ((getTick() - k->last_ecm > 1000) && !pmt->cw)
		k->ecm_parity[i] = -1;

	if ((b[0] == 0x80 || b[0] == 0x81) && (b[0] & 1) == k->ecm_parity[i])
		return 0;

	old_parity = k->ecm_parity[i];
	k->ecm_parity[i] = b[0] & 1;

	len = ((b[1] & 0xF) << 8) + b[2];
	len += 3;
	k->last_ecm = getTick();
	LOG(
		"dvbapi: sending ECM key %d for pid %04X (%d), current ecm_parity = %d, previous parity %d, demux = %d, filter = %d, len = %d [%02X %02X %02X %02X]",
		k->id, pid, pid, old_parity, k->ecm_parity[i], demux, filter, len, b[0], b[1], b[2], b[3]);

	if (demux < 0)
		return 0;

	if (len > 559 + 3)
		return -1;

	copy32(buf, 0, DVBAPI_FILTER_DATA);
	buf[4] = demux;
	buf[5] = filter;
	memcpy(buf + 6, b, len);
	//	hexdump("ecm: ", buf, len + 6);
	TEST_WRITE(write(sock, buf, len + 6), len + 6);
	return 0;
}

int set_algo(SKey *k, int algo, int mode)
{
	if (algo == CA_ALGO_AES128 && mode == CA_MODE_CBC)
		algo = CA_ALGO_AES128_CBC;

	k->algo = algo;

	return 0;
}

int keys_add(int i, int adapter, int pmt_id)
{

	SKey *k;
	SPMT *pmt = get_pmt(pmt_id);
	if (!pmt)
		LOG_AND_RETURN(-1, "%s: PMT %d not found ", __FUNCTION__, pmt_id);
	if (i == -1)
		i = add_new_lock((void **)keys, MAX_KEYS, sizeof(SKey), &keys_mutex);
	else
	{
		if (keys[i])
			mutex_lock(&keys[i]->mutex);
		else
		{
			keys[i] = malloc(sizeof(SKey));
			if (!keys[i])
				LOG_AND_RETURN(-1, "Could not allocate memory for the key %d", i);
			memset(keys[i], 0, sizeof(SKey));
			mutex_init(&keys[i]->mutex);
			mutex_lock(&keys[i]->mutex);
		}
	}
	if (i == -1 || !keys[i])
	{
		LOG_AND_RETURN(-1, "Key buffer is full, could not add new keys");
	}

	k = keys[i];

	k->parity = -1;
	k->sid = pmt->sid;
	k->pmt_id = pmt_id;
	k->adapter = adapter;
	k->id = i;
	k->blen = 0;
	k->enabled = 1;
	k->ver = -1;
	k->ecms = 0;
	k->last_dmx_stop = 0;
	memset(k->cw[0], 0, 16);
	memset(k->cw[1], 0, 16);
	memset(k->filter_id, -1, sizeof(k->filter_id));
	memset(k->filter, -1, sizeof(k->filter));
	memset(k->demux, -1, sizeof(k->demux));
	mutex_unlock(&k->mutex);
	invalidate_adapter(adapter);
	enabledKeys++;
	LOG("returning new key %d for adapter %d, pmt %d pid %d sid %04X", i, adapter,
		pmt->id, pmt->pid, k->sid);

	return i;
}

int keys_del(int i)
{
	int j, ek;
	SKey *k;
	unsigned char buf[8] =
		{0x9F, 0x80, 0x3f, 4, 0x83, 2, 0, 0};
	k = get_key(i);
	if (!k)
		return 0;

	mutex_lock(&k->mutex);
	if (!k->enabled)
	{
		mutex_unlock(&k->mutex);
		return 0;
	}
	k->enabled = 0;
	//	buf[7] = k->demux;
	buf[7] = i;
	LOG("Stopping DEMUX %d, removing key %d, sock %d, pmt pid %d", buf[7], i,
		sock, k->pmt_pid);
	if ((buf[7] != 255) && (sock > 0))
		TEST_WRITE(write(sock, buf, sizeof(buf)), sizeof(buf));

	k->sid = 0;
	k->pmt_pid = 0;
	k->adapter = -1;
	k->last_dmx_stop = 0;
	for (j = 0; j < MAX_KEY_FILTERS; j++)
		if (k->filter_id[j] >= 0)
			del_filter(k->filter_id[j]);

	ek = 0;
	k->hops = k->caid = k->info_pid = k->prid = k->ecmtime = 0;
	buf[7] = 0xFF;
	for (j = 0; j < MAX_KEYS; j++)
		if (keys[j] && keys[j]->enabled)
			ek++;
	enabledKeys = ek;
	if (!ek && sock > 0)
		TEST_WRITE(write(sock, buf, sizeof(buf)), sizeof(buf));
	mutex_destroy(&k->mutex);
	return 0;
}

int dvbapi_add_pmt(adapter *ad, SPMT *pmt)
{
	SKey *k = NULL;
	SPid *p;
	int key, pid = pmt->pid;
	p = find_pid(ad->id, pid);
	if (!p)
		return 1;

	key = keys_add(-1, ad->id, pmt->id);
	k = get_key(key);
	if (!k)
		LOG_AND_RETURN(1, "Could not add key for pmt %d", pmt->id);
	mutex_lock(&k->mutex);
	pmt->opaque = k;
	k->pi_len = pmt->pi_len;
	k->pi = pmt->pi;
	k->sid = pmt->sid;
	k->adapter = ad->id;
	k->pmt_pid = pid;
	dvbapi_send_pmt(k);
	//	if (p->key != spmt->old_key)
	//		set_next_key(p->key, spmt->old_key);
	mutex_unlock(&k->mutex);
	return 0;
}

int dvbapi_del_pmt(adapter *ad, SPMT *pmt)
{
	SKey *k = (SKey *)pmt->opaque;
	keys_del(k->id);
	LOG("%s: deleted PMT pid %d, id %d", __FUNCTION__, pmt->pid, pmt->id);
	return 0;
}

int dvbapi_init_dev(adapter *ad)
{
	return TABLES_RESULT_OK;
}

SCA_op dvbapi;

void register_dvbapi()
{
	memset(&dvbapi, 0, sizeof(dvbapi));
	dvbapi.ca_init_dev = dvbapi_init_dev;
	dvbapi.ca_add_pmt = dvbapi_add_pmt;
	dvbapi.ca_del_pmt = dvbapi_del_pmt;
	dvbapi_ca = add_ca(&dvbapi, 0xFFFFFFFF);
}

void unregister_dvbapi()
{
	LOG("unregistering dvbapi as the socket is closed");
	del_ca(&dvbapi);
	dvbapi_ca = -1;
}

void dvbapi_delete_keys_for_adapter(int aid)
{
	int i;
	SKey *k;
	for (i = 0; i < MAX_KEYS; i++)
		if ((k = get_key(i)) && k->adapter == aid)
			keys_del(i);
}

char *get_channel_for_key(int key, char *dest, int max_size)
{
	SKey *k = get_key(key);
	SPMT *pmt = NULL;
	dest[0] = 0;
	dest[max_size - 1] = 0;
	if (k)
		pmt = get_pmt(k->pmt_id);
	if (pmt)
		strncpy(dest, pmt->name, max_size - 1);

	return dest;
}

void free_all_keys(void)
{
	int i;
	for (i = 0; i < MAX_KEYS; i++)
	{
		if (keys[i])
		{
			mutex_destroy(&keys[i]->mutex);
			free(keys[i]);
		}
	}
	mutex_destroy(&keys_mutex);
}

_symbols dvbapi_sym[] =
	{
		{"key_enabled", VAR_AARRAY_INT8, keys, 1, MAX_KEYS, offsetof(SKey, enabled)},
		{"key_hops", VAR_AARRAY_INT8, keys, 1, MAX_KEYS, offsetof(SKey, hops)},
		{"key_ecmtime", VAR_AARRAY_INT, keys, 1, MAX_KEYS, offsetof(SKey, ecmtime)},
		{"key_pmt", VAR_AARRAY_INT, keys, 1, MAX_KEYS, offsetof(SKey, pmt_pid)},
		{"key_adapter", VAR_AARRAY_INT, keys, 1, MAX_KEYS, offsetof(SKey, adapter)},
		{"key_cardsystem", VAR_AARRAY_STRING, keys, 1, MAX_KEYS, offsetof(SKey,
																		  cardsystem)},
		{"key_reader", VAR_AARRAY_STRING, keys, 1, MAX_KEYS, offsetof(SKey, reader)},
		{"key_from", VAR_AARRAY_STRING, keys, 1, MAX_KEYS, offsetof(SKey, from)},
		{"key_protocol", VAR_AARRAY_STRING, keys, 1, MAX_KEYS, offsetof(SKey,
																		protocol)},
		{"key_channel", VAR_FUNCTION_STRING, (void *)&get_channel_for_key, 0, MAX_KEYS, 0},

		{NULL, 0, NULL, 0, 0}};
