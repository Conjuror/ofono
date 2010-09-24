/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/stk.h>

#include "gatchat.h"
#include "gatresult.h"

#include "ifxmodem.h"

struct stk_data {
	GAtChat *chat;
};

static const char *none_prefix[] = { NULL };
static const char *sate_prefix[] = { "+SATE:", NULL };
static const char *stkprof_prefix[] = { "+STKPROF:", NULL };

static void sate_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_envelope_cb_t cb = cbd->cb;
	GAtResultIter iter;
	struct ofono_error error;
	int sw1, sw2, envelope, event;
	const guint8 *pdu = NULL;
	gint len = 0;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, NULL, 0, cbd->data);
		return;
	}

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+SATE:") == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &sw1) == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &sw2) == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &envelope) == FALSE)
		goto error;

	if (g_at_result_iter_next_number(&iter, &event) == FALSE)
		goto error;

	DBG("sw1 %d sw2 %d envelope %d event %d", sw1, sw2, envelope, event);

	/* Response data is optional */
	g_at_result_iter_next_hexstring(&iter, &pdu, &len);

	DBG("len %d", len);

	cb(&error, pdu, len, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, NULL, 0, cbd->data);
}

static void ifx_stk_envelope(struct ofono_stk *stk, int length,
				const unsigned char *command,
				ofono_stk_envelope_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 64 + length * 2);
	int len;

	DBG("");

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+SATE=\"");
	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *command++);
	len += sprintf(buf + len, "\"");

	DBG("%s", buf);

	if (g_at_chat_send(sd->chat, buf, sate_prefix,
					sate_cb, cbd, g_free) > 0) {
		g_free(buf);
		return;
	}

error:
	g_free(buf);
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, NULL, 0, data);
}

static void satr_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_stk_generic_cb_t cb = cbd->cb;
	struct ofono_error error;

	DBG("");

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void ifx_stk_terminal_response(struct ofono_stk *stk, int length,
					const unsigned char *command,
					ofono_stk_generic_cb_t cb, void *data)
{
	struct stk_data *sd = ofono_stk_get_data(stk);
	struct cb_data *cbd = cb_data_new(cb, data);
	char *buf = g_try_new(char, 64 + length * 2);
	int len;

	DBG("");

	if (!cbd || !buf)
		goto error;

	len = sprintf(buf, "AT+SATR=\"");
	for (; length; length--)
		len += sprintf(buf + len, "%02hhX", *command++);
	len += sprintf(buf + len, "\"");

	DBG("%s", buf);

	if (g_at_chat_send(sd->chat, buf, none_prefix,
					satr_cb, cbd, g_free) > 0) {
		g_free(buf);
		return;
	}

error:
	g_free(buf);
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void sati_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GAtResultIter iter;
	const guint8 *pdu;
	gint len;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+SATI:") == FALSE)
		return;

	if (g_at_result_iter_next_hexstring(&iter, &pdu, &len) == FALSE)
		return;

	DBG("len %d", len);

	ofono_stk_proactive_command_notify(stk, len, pdu);
}

static void satn_notify(GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;
	const guint8 *pdu;
	gint len;

	DBG("");

	if (g_at_result_iter_next(&iter, "+SATN:") == FALSE)
		return;

	if (g_at_result_iter_next_hexstring(&iter, &pdu, &len) == FALSE)
		return;

	DBG("len %d", len);

	/* Proactive command has been handled by the modem.  If the
	 * command was for Setup Call then a response with AT+SATD
	 * is required.  This is not handled properly yet.
	 */
}

static void satf_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	GAtResultIter iter;
	int sw1, sw2;

	DBG("");

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+SATF:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &sw1) == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &sw2) == FALSE)
		return;

	DBG("sw1 %d sw2 %d", sw1, sw2);

	ofono_stk_proactive_session_end_notify(stk);
}

static void stkprof_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_stk *stk = user_data;
	struct stk_data *sd = ofono_stk_get_data(stk);

	DBG("");

	if (!ok)
		return;

	g_at_chat_register(sd->chat, "+SATI:", sati_notify, FALSE, stk, NULL);
	g_at_chat_register(sd->chat, "+SATN:", satn_notify, FALSE, stk, NULL);
	g_at_chat_register(sd->chat, "+SATF:", satf_notify, FALSE, stk, NULL);

	g_at_chat_send(sd->chat, "AT+STKPROF?", stkprof_prefix,
							NULL, NULL, NULL);

	g_at_chat_send(sd->chat, "AT+STKPROF=4,\"1F7F\"", none_prefix,
							NULL, NULL, NULL);

	ofono_stk_register(stk);
}

static int ifx_stk_probe(struct ofono_stk *stk, unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct stk_data *sd;

	DBG("");

	sd = g_try_new0(struct stk_data, 1);
	if (!sd)
		return -ENOMEM;

	sd->chat = g_at_chat_clone(chat);

	ofono_stk_set_data(stk, sd);

	g_at_chat_send(sd->chat, "AT+STKPROF=?", stkprof_prefix,
					stkprof_support_cb, stk, NULL);

	return 0;
}

static void ifx_stk_remove(struct ofono_stk *stk)
{
	struct stk_data *sd = ofono_stk_get_data(stk);

	DBG("");

	ofono_stk_set_data(stk, NULL);

	g_at_chat_unref(sd->chat);
	g_free(sd);
}

static struct ofono_stk_driver driver = {
	.name			= "ifxmodem",
	.probe			= ifx_stk_probe,
	.remove			= ifx_stk_remove,
	.envelope		= ifx_stk_envelope,
	.terminal_response	= ifx_stk_terminal_response,
};

void ifx_stk_init()
{
	ofono_stk_driver_register(&driver);
}

void ifx_stk_exit()
{
	ofono_stk_driver_unregister(&driver);
}