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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/phonebook.h>
#include <ofono/log.h>

#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };

struct zte_data {
	GAtChat *modem;
	GAtChat *aux;
};

static int zte_probe(struct ofono_modem *modem)
{
	struct zte_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct zte_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void zte_remove(struct ofono_modem *modem)
{
	struct zte_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->modem);
	g_at_chat_unref(data->aux);

	g_free(data);
}

static void zte_debug(const char *str, void *user_data)
{
        const char *prefix = user_data;
        ofono_info("%s%s", prefix, str);
}

static GAtChat *open_device(struct ofono_modem *modem,
				const char *key, char *debug)
{
	const char *device;
	GAtSyntax *syntax;
	GIOChannel *channel;
	GAtChat *chat;

	device = ofono_modem_get_string(modem, key);
	if (device == NULL)
		return NULL;

	DBG("%s %s", key, device);

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return NULL;

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!chat)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, zte_debug, debug);

	return chat;
}

static void zte_disconnect(gpointer user_data)
{
	DBG("");
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	ofono_modem_set_powered(modem, ok);
}

static int zte_enable(struct ofono_modem *modem)
{
	struct zte_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	data->modem = open_device(modem, "Modem", "Modem:");
	if (data->modem == NULL)
		return -EINVAL;

	g_at_chat_set_disconnect_function(data->modem,
						zte_disconnect, modem);

	data->aux = open_device(modem, "Aux", "Aux:");
	if (data->aux == NULL) {
		g_at_chat_unref(data->modem);
		data->modem = NULL;
		return -EIO;
	}

	g_at_chat_send(data->aux, "ATE0 +CMEE=1", none_prefix,
						NULL, NULL, NULL);

	g_at_chat_send(data->aux, "AT+CFUN=1", none_prefix,
					cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct zte_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->aux);
	data->aux = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int zte_disable(struct ofono_modem *modem)
{
	struct zte_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->modem) {
		g_at_chat_cancel_all(data->modem);
		g_at_chat_unregister_all(data->modem);
		g_at_chat_unref(data->modem);
		data->modem = NULL;
	}

	if (!data->aux)
		return 0;

	g_at_chat_cancel_all(data->aux);
	g_at_chat_unregister_all(data->aux);
	g_at_chat_send(data->aux, "AT+CFUN=0", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void zte_pre_sim(struct ofono_modem *modem)
{
	struct zte_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->aux);
	sim = ofono_sim_create(modem, 0, "atmodem", data->aux);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void zte_post_sim(struct ofono_modem *modem)
{
	struct zte_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_ZTE, "atmodem", data->aux);

	ofono_sms_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
					"atmodem", data->aux);
	ofono_cbs_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
					"atmodem", data->aux);
	ofono_ussd_create(modem, OFONO_VENDOR_QUALCOMM_MSM,
					"atmodem", data->aux);
	ofono_phonebook_create(modem, 0, "atmodem", data->aux);
}

static struct ofono_modem_driver zte_driver = {
	.name		= "zte",
	.probe		= zte_probe,
	.remove		= zte_remove,
	.enable		= zte_enable,
	.disable	= zte_disable,
	.pre_sim	= zte_pre_sim,
	.post_sim	= zte_post_sim,
};

static int zte_init(void)
{
	return ofono_modem_driver_register(&zte_driver);
}

static void zte_exit(void)
{
	ofono_modem_driver_unregister(&zte_driver);
}

OFONO_PLUGIN_DEFINE(zte, "ZTE modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, zte_init, zte_exit)