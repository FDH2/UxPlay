/**
 *  Copyright (C) 2018  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *===================================================================
 * modfied by fduncanh 2021-2023
 */

/* This file should be only included from raop.c as it defines static handler
 * functions and depends on raop internals */

#include "dnssdint.h"
#include "utils.h"
#include <ctype.h>
#include <stdlib.h>
#include <plist/plist.h>
#define AUDIO_SAMPLE_RATE 44100   /* all supported AirPlay audio format use this sample rate */
#define SECOND_IN_USECS 1000000

typedef void (*raop_handler_t)(raop_conn_t *, http_request_t *,
                               http_response_t *, char **, int *);

static void
raop_handler_info(raop_conn_t *conn,
                  http_request_t *request, http_response_t *response,
                  char **response_data, int *response_datalen)
{
    assert(conn->raop->dnssd);

    plist_t res_node = plist_new_dict();

    /* deviceID is the physical hardware address, and will not change */
    int hw_addr_raw_len = 0;
    const char *hw_addr_raw = dnssd_get_hw_addr(conn->raop->dnssd, &hw_addr_raw_len);
    char *hw_addr = calloc(1, 3 * hw_addr_raw_len);
    //int hw_addr_len =
    utils_hwaddr_airplay(hw_addr, 3 * hw_addr_raw_len, hw_addr_raw, hw_addr_raw_len);
    plist_t device_id_node = plist_new_string(hw_addr);
    plist_dict_set_item(res_node, "deviceID", device_id_node);

    /* Persistent Public Key */
    int pk_len = 0;
    char *pk = utils_parse_hex(conn->raop->pk_str, strlen(conn->raop->pk_str), &pk_len);
    plist_t pk_node = plist_new_data(pk, pk_len);
    plist_dict_set_item(res_node, "pk", pk_node);

    /* airplay_txt is from the _airplay._tcp  dnssd announuncement, may not be necessary */
    int airplay_txt_len = 0;
    const char *airplay_txt = dnssd_get_airplay_txt(conn->raop->dnssd, &airplay_txt_len);
    plist_t txt_airplay_node = plist_new_data(airplay_txt, airplay_txt_len);
    plist_dict_set_item(res_node, "txtAirPlay", txt_airplay_node);

    uint64_t features = dnssd_get_airplay_features(conn->raop->dnssd);
    plist_t features_node = plist_new_uint(features);
    plist_dict_set_item(res_node, "features", features_node);

    int name_len = 0;
    const char *name = dnssd_get_name(conn->raop->dnssd, &name_len);
    plist_t name_node = plist_new_string(name);
    plist_dict_set_item(res_node, "name", name_node);

    plist_t audio_latencies_node = plist_new_array();
    plist_t audio_latencies_0_node = plist_new_dict();
    plist_t audio_latencies_0_output_latency_micros_node = plist_new_bool(0);
    plist_t audio_latencies_0_type_node = plist_new_uint(100);
    plist_t audio_latencies_0_audio_type_node = plist_new_string("default");
    plist_t audio_latencies_0_input_latency_micros_node = plist_new_uint(0);
    plist_dict_set_item(audio_latencies_0_node, "type", audio_latencies_0_type_node);
    plist_dict_set_item(audio_latencies_0_node, "inputLatencyMicros", audio_latencies_0_input_latency_micros_node);
    plist_dict_set_item(audio_latencies_0_node, "audioType", audio_latencies_0_audio_type_node);
    plist_dict_set_item(audio_latencies_0_node, "outputLatencyMicros", audio_latencies_0_output_latency_micros_node);
    plist_array_append_item(audio_latencies_node, audio_latencies_0_node);

    plist_t audio_latencies_1_node = plist_new_dict();
    plist_t audio_latencies_1_output_latency_micros_node = plist_new_bool(0);
    plist_t audio_latencies_1_type_node = plist_new_uint(101);
    plist_t audio_latencies_1_audio_type_node = plist_new_string("default");
    plist_t audio_latencies_1_input_latency_micros_node = plist_new_uint(0);
    plist_dict_set_item(audio_latencies_1_node, "type", audio_latencies_1_type_node);
    plist_dict_set_item(audio_latencies_1_node, "audioType", audio_latencies_1_audio_type_node);
    plist_dict_set_item(audio_latencies_1_node, "inputLatencyMicros", audio_latencies_1_input_latency_micros_node);
    plist_dict_set_item(audio_latencies_1_node, "outputLatencyMicros", audio_latencies_1_output_latency_micros_node);
    plist_array_append_item(audio_latencies_node, audio_latencies_1_node);
    plist_dict_set_item(res_node, "audioLatencies", audio_latencies_node);

    plist_t audio_formats_node = plist_new_array();
    plist_t audio_format_0_node = plist_new_dict();
    plist_t audio_format_0_type_node = plist_new_uint(100);
    plist_t audio_format_0_audio_input_formats_node = plist_new_uint(0x3fffffc);
    plist_t audio_format_0_audio_output_formats_node = plist_new_uint(0x3fffffc);
    plist_dict_set_item(audio_format_0_node, "audioOutputFormats", audio_format_0_audio_output_formats_node);
    plist_dict_set_item(audio_format_0_node, "type", audio_format_0_type_node);
    plist_dict_set_item(audio_format_0_node, "audioInputFormats", audio_format_0_audio_input_formats_node);
    plist_array_append_item(audio_formats_node, audio_format_0_node);

    plist_t audio_format_1_node = plist_new_dict();
    plist_t audio_format_1_type_node = plist_new_uint(101);
    plist_t audio_format_1_audio_input_formats_node = plist_new_uint(0x3fffffc);
    plist_t audio_format_1_audio_output_formats_node = plist_new_uint(0x3fffffc);
    plist_dict_set_item(audio_format_1_node, "audioOutputFormats", audio_format_1_audio_output_formats_node);
    plist_dict_set_item(audio_format_1_node, "type", audio_format_1_type_node);
    plist_dict_set_item(audio_format_1_node, "audioInputFormats", audio_format_1_audio_input_formats_node);
    plist_array_append_item(audio_formats_node, audio_format_1_node);
    plist_dict_set_item(res_node, "audioFormats", audio_formats_node);

    plist_t pi_node = plist_new_string(AIRPLAY_PI);
    plist_dict_set_item(res_node, "pi", pi_node);

    plist_t vv_node = plist_new_uint(strtol(AIRPLAY_VV, NULL, 10));
    plist_dict_set_item(res_node, "vv", vv_node);

    plist_t status_flags_node = plist_new_uint(68);
    plist_dict_set_item(res_node, "statusFlags", status_flags_node);

    plist_t keep_alive_low_power_node = plist_new_uint(1);
    plist_dict_set_item(res_node, "keepAliveLowPower", keep_alive_low_power_node);

    plist_t source_version_node = plist_new_string(GLOBAL_VERSION);
    plist_dict_set_item(res_node, "sourceVersion", source_version_node);

    plist_t keep_alive_send_stats_as_body_node = plist_new_bool(1);
    plist_dict_set_item(res_node, "keepAliveSendStatsAsBody", keep_alive_send_stats_as_body_node);

    plist_t model_node = plist_new_string(GLOBAL_MODEL);
    plist_dict_set_item(res_node, "model", model_node);

    plist_t mac_address_node = plist_new_string(hw_addr);
    plist_dict_set_item(res_node, "macAddress", mac_address_node);

    plist_t displays_node = plist_new_array();
    plist_t displays_0_node = plist_new_dict();
    plist_t displays_0_width_physical_node = plist_new_uint(0);
    plist_t displays_0_height_physical_node = plist_new_uint(0);
    plist_t displays_0_uuid_node = plist_new_string("e0ff8a27-6738-3d56-8a16-cc53aacee925");
    plist_t displays_0_width_node = plist_new_uint(conn->raop->width);
    plist_t displays_0_height_node = plist_new_uint(conn->raop->height);
    plist_t displays_0_width_pixels_node = plist_new_uint(conn->raop->width);
    plist_t displays_0_height_pixels_node = plist_new_uint(conn->raop->height);
    plist_t displays_0_rotation_node = plist_new_bool(0); /* set to true in AppleTV gen 3 (which has features bit 8  set */
    plist_t displays_0_refresh_rate_node = plist_new_real((double) 1.0 / conn->raop->refreshRate);  /* set as real 0.166666  = 60hz in AppleTV gen 3 */
    plist_t displays_0_max_fps_node = plist_new_uint(conn->raop->maxFPS);
    plist_t displays_0_overscanned_node = plist_new_bool(conn->raop->overscanned);
    plist_t displays_0_features = plist_new_uint(14);

    plist_dict_set_item(displays_0_node, "uuid", displays_0_uuid_node);
    plist_dict_set_item(displays_0_node, "widthPhysical", displays_0_width_physical_node);
    plist_dict_set_item(displays_0_node, "heightPhysical", displays_0_height_physical_node);
    plist_dict_set_item(displays_0_node, "width", displays_0_width_node);
    plist_dict_set_item(displays_0_node, "height", displays_0_height_node);
    plist_dict_set_item(displays_0_node, "widthPixels", displays_0_width_pixels_node);
    plist_dict_set_item(displays_0_node, "heightPixels", displays_0_height_pixels_node);
    plist_dict_set_item(displays_0_node, "rotation", displays_0_rotation_node);    
    plist_dict_set_item(displays_0_node, "refreshRate", displays_0_refresh_rate_node);
    plist_dict_set_item(displays_0_node, "maxFPS", displays_0_max_fps_node);
    plist_dict_set_item(displays_0_node, "overscanned", displays_0_overscanned_node);
    plist_dict_set_item(displays_0_node, "features", displays_0_features);
    plist_array_append_item(displays_node, displays_0_node);
    plist_dict_set_item(res_node, "displays", displays_node);

    plist_to_bin(res_node, response_data, (uint32_t *) response_datalen);
    plist_free(res_node);
    http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
    free(pk);
    free(hw_addr);
}

static void
raop_handler_pairpinstart(raop_conn_t *conn,
                          http_request_t *request, http_response_t *response,
                          char **response_data, int *response_datalen) {
    logger_log(conn->raop->logger, LOGGER_INFO, "client sent PAIR-PIN-START request");
    int pin_4;
    if (conn->raop->pin > 9999) {
        pin_4 = conn->raop->pin % 10000;
    } else {
        pin_4 = random_pin();
        if (pin_4 < 0) {
            logger_log(conn->raop->logger, LOGGER_ERR, "Failed to generate random pin");
        } else {
            conn->raop->pin = (unsigned short) pin_4 % 10000;
        }
    }
    char pin[6];
    snprintf(pin, 5, "%04u", pin_4);
    if (conn->raop->callbacks.display_pin) {
         conn->raop->callbacks.display_pin(conn->raop->callbacks.cls, pin);
    }
    logger_log(conn->raop->logger, LOGGER_INFO, "*** CLIENT MUST NOW ENTER PIN = \"%s\" AS AIRPLAY PASSWORD", pin);
    *response_data = NULL;
    response_datalen = 0;
}

static void
raop_handler_pairsetup_pin(raop_conn_t *conn,
                           http_request_t *request, http_response_t *response,
                           char **response_data, int *response_datalen) {

    const char *request_data = NULL;;
    int request_datalen = 0;
    bool data_is_plist = false;
    bool logger_debug = (logger_get_level(conn->raop->logger) >= LOGGER_DEBUG);
    request_data = http_request_get_data(request, &request_datalen);
    logger_log(conn->raop->logger, LOGGER_INFO, "client requested pair-setup-pin, datalen = %d", request_datalen);
    if (request_datalen > 0) {
        char *header_str= NULL; 
        http_request_get_header_string(request, &header_str);
        logger_log(conn->raop->logger, LOGGER_INFO, "request header: %s", header_str);
        data_is_plist = (strstr(header_str,"apple-binary-plist") != NULL);
        free(header_str);
    }
    if (!data_is_plist) {
        logger_log(conn->raop->logger, LOGGER_INFO, "did not receive expected plist from client, request_datalen = %d",
                  request_datalen);
        goto authentication_failed;
    }

    /* process the pair-setup-pin request */
    plist_t req_root_node = NULL;
    plist_from_bin(request_data, request_datalen, &req_root_node);
    plist_t req_method_node = plist_dict_get_item(req_root_node, "method");
    plist_t req_user_node = plist_dict_get_item(req_root_node, "user");
    plist_t req_pk_node = plist_dict_get_item(req_root_node, "pk");
    plist_t req_proof_node = plist_dict_get_item(req_root_node, "proof");
    plist_t req_epk_node = plist_dict_get_item(req_root_node, "epk");
    plist_t req_authtag_node = plist_dict_get_item(req_root_node, "authTag");
  
    if (PLIST_IS_STRING(req_method_node) && PLIST_IS_STRING(req_user_node)) {
        /* this is the initial pair-setup-pin request */
        const char *salt;
	char pin[6];
	const char *pk;
	int len_pk, len_salt;
        char *method = NULL;
        char *user = NULL;
        plist_get_string_val(req_method_node, &method);
        if (strncmp(method, "pin", strlen (method))) {
            logger_log(conn->raop->logger, LOGGER_ERR, "error, required method is \"pin\", client requested \"%s\"", method);
            *response_data = NULL;
            response_datalen = 0;
	    free (method);
	    plist_free (req_root_node);
            return;
        }
        free (method);
	plist_get_string_val(req_user_node, &user);
        logger_log(conn->raop->logger, LOGGER_INFO, "pair-setup-pin:  device_id = %s", user);
        snprintf(pin, 6, "%04u", conn->raop->pin % 10000);
        if (conn->raop->pin < 10000) {
            conn->raop->pin = 0;
        }
	int ret = srp_new_user(conn->session, conn->raop->pairing, (const char *) user,
                               (const char *) pin, &salt, &len_salt, &pk, &len_pk);
        free(user);	
        plist_free(req_root_node);
        if (ret < 0) {
            logger_log(conn->raop->logger, LOGGER_ERR, "failed to create user, err = %d", ret);
            goto authentication_failed;
        }
        plist_t res_root_node = plist_new_dict();
        plist_t res_salt_node = plist_new_data(salt, len_salt);
        plist_t res_pk_node = plist_new_data(pk, len_pk);
        plist_dict_set_item(res_root_node, "pk", res_pk_node);	
        plist_dict_set_item(res_root_node, "salt", res_salt_node);
        plist_to_bin(res_root_node, response_data, (uint32_t*) response_datalen);
        plist_free(res_root_node);
        http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
	return;
    } else if (PLIST_IS_DATA(req_pk_node) && PLIST_IS_DATA(req_proof_node)) {
        /* this is the second part of pair-setup-pin request */
        char *client_pk = NULL;
        char *client_proof = NULL;
	unsigned char proof[64];
	memset(proof, 0, sizeof(proof));
        uint64_t client_pk_len;
        uint64_t client_proof_len;
        plist_get_data_val(req_pk_node, &client_pk, &client_pk_len); 
        plist_get_data_val(req_proof_node, &client_proof, &client_proof_len);
        if (logger_debug) {
	  char *str = utils_data_to_string((const unsigned char *) client_proof, client_proof_len, 20);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "client SRP6a proof <M> :\n%s", str);	    
            free (str);
        }
        memcpy(proof, client_proof, (int) client_proof_len);
        free (client_proof);
        int ret = srp_validate_proof(conn->session, conn->raop->pairing, (const unsigned char *) client_pk,
                                     (int) client_pk_len, proof, (int) client_proof_len, (int) sizeof(proof));
        free (client_pk);
        plist_free(req_root_node);
        if (ret < 0) {
            logger_log(conn->raop->logger, LOGGER_ERR, "Client Authentication Failure (client proof not validated)");
            goto authentication_failed;
        }
        if (logger_debug) {
	    char *str = utils_data_to_string((const unsigned char *) proof, sizeof(proof), 20);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "server SRP6a proof <M1> :\n%s", str);
            free (str);
        }
        plist_t res_root_node = plist_new_dict();
        plist_t res_proof_node = plist_new_data((const char *) proof, 20);
        plist_dict_set_item(res_root_node, "proof", res_proof_node);	
        plist_to_bin(res_root_node, response_data, (uint32_t*) response_datalen);
        plist_free(res_root_node);
        http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
	return;
    } else if (PLIST_IS_DATA(req_epk_node) && PLIST_IS_DATA(req_authtag_node)) {
        /* this is the third part of pair-setup-pin request */
        char *client_epk = NULL;
        char *client_authtag = NULL;
        uint64_t client_epk_len;
        uint64_t client_authtag_len;
        unsigned char epk[ED25519_KEY_SIZE];
	unsigned char authtag[GCM_AUTHTAG_SIZE];
	int ret;
        plist_get_data_val(req_epk_node, &client_epk, &client_epk_len); 
        plist_get_data_val(req_authtag_node, &client_authtag, &client_authtag_len);

	if (logger_debug) {
            char *str = utils_data_to_string((const unsigned char *) client_epk, client_epk_len, 16);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "client_epk %d:\n%s\n", (int) client_epk_len, str);
            str = utils_data_to_string((const unsigned char *) client_authtag, client_authtag_len, 16);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "client_authtag  %d:\n%s\n", (int) client_authtag_len, str);
            free (str);
	}

	memcpy(epk, client_epk, ED25519_KEY_SIZE);
	memcpy(authtag, client_authtag, GCM_AUTHTAG_SIZE);
        free (client_authtag);
        free (client_epk);
        plist_free(req_root_node);
	ret = srp_confirm_pair_setup(conn->session, conn->raop->pairing, epk, authtag);
        if (ret < 0) {
            logger_log(conn->raop->logger, LOGGER_ERR, "pair-pin-setup (step 3): client authentication failed\n");
            goto authentication_failed;
        } else {
            logger_log(conn->raop->logger, LOGGER_DEBUG, "pair-pin-setup success\n");
        }
        pairing_session_set_setup_status(conn->session);
        plist_t res_root_node = plist_new_dict();
        plist_t res_epk_node = plist_new_data((const char *) epk, 32);
	plist_t res_authtag_node = plist_new_data((const char *) authtag, 16);
        plist_dict_set_item(res_root_node, "epk", res_epk_node);
        plist_dict_set_item(res_root_node, "authTag", res_authtag_node);	
        plist_to_bin(res_root_node, response_data, (uint32_t*) response_datalen);
        plist_free(res_root_node);
        http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
	return;
    }
 authentication_failed:;
    http_response_init(response, "RTSP/1.0", 470, "Client Authentication Failure");
}

static void
raop_handler_pairsetup(raop_conn_t *conn,
                       http_request_t *request, http_response_t *response,
                       char **response_data, int *response_datalen)
{
    unsigned char public_key[ED25519_KEY_SIZE];
    //const char *data;
    int datalen;

    //data =
    http_request_get_data(request, &datalen);
    if (datalen != 32) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Invalid pair-setup data");
        return;
    }

    pairing_get_public_key(conn->raop->pairing, public_key);
    pairing_session_set_setup_status(conn->session);

    *response_data = malloc(sizeof(public_key));
    if (*response_data) {
        http_response_add_header(response, "Content-Type", "application/octet-stream");
        memcpy(*response_data, public_key, sizeof(public_key));
        *response_datalen = sizeof(public_key);
    }
}

static void
raop_handler_pairverify(raop_conn_t *conn,
                        http_request_t *request, http_response_t *response,
                        char **response_data, int *response_datalen)
{
    bool register_check = false;  
    if (pairing_session_check_handshake_status(conn->session)) {
        if (conn->raop->use_pin) {
            pairing_session_set_setup_status(conn->session);
            register_check = true;
        } else {
            return;
        }
    }
    unsigned char public_key[X25519_KEY_SIZE];
    unsigned char signature[PAIRING_SIG_SIZE];
    const unsigned char *data;
    int datalen;

    data = (unsigned char *) http_request_get_data(request, &datalen);
    if (datalen < 4) {
        logger_log(conn->raop->logger, LOGGER_ERR, "Invalid pair-verify data");
        return;
    }
    switch (data[0]) {
        case 1:
            if (datalen != 4 + X25519_KEY_SIZE + ED25519_KEY_SIZE) {
                logger_log(conn->raop->logger, LOGGER_ERR, "Invalid pair-verify data");
                return;
            }
            /* We can fall through these errors, the result will just be garbage... */
            if (pairing_session_handshake(conn->session, data + 4, data + 4 + X25519_KEY_SIZE)) {
                logger_log(conn->raop->logger, LOGGER_ERR, "Error initializing pair-verify handshake");
            }
            if (pairing_session_get_public_key(conn->session, public_key)) {
                logger_log(conn->raop->logger, LOGGER_ERR, "Error getting ECDH public key");
            }
            if (pairing_session_get_signature(conn->session, signature)) {
                logger_log(conn->raop->logger, LOGGER_ERR, "Error getting ED25519 signature");
            }
            if (register_check) {
                bool registered_client = true;
		if (conn->raop->callbacks.check_register) {
		    const unsigned char *pk = data + 4 + X25519_KEY_SIZE;
		    char *pk64;
		    ed25519_pk_to_base64(pk, &pk64);
                    registered_client = conn->raop->callbacks.check_register(conn->raop->callbacks.cls, pk64);
		    free (pk64);
                }

                if (!registered_client) {
                    return;
                }
            }
            *response_data = malloc(sizeof(public_key) + sizeof(signature));
            if (*response_data) {
                http_response_add_header(response, "Content-Type", "application/octet-stream");
                memcpy(*response_data, public_key, sizeof(public_key));
                memcpy(*response_data + sizeof(public_key), signature, sizeof(signature));
                *response_datalen = sizeof(public_key) + sizeof(signature);
            }
            break;
        case 0:
            logger_log(conn->raop->logger, LOGGER_DEBUG, "2nd pair-verify step: checking signature");
            if (datalen != 4 + PAIRING_SIG_SIZE) {
                logger_log(conn->raop->logger, LOGGER_ERR, "Invalid pair-verify data");
                return;
            }

            if (pairing_session_finish(conn->session, data + 4)) {
                logger_log(conn->raop->logger, LOGGER_ERR, "Incorrect pair-verify signature");
                http_response_set_disconnect(response, 1);
                return;
            }
            logger_log(conn->raop->logger, LOGGER_DEBUG, "pair-verify: signature is verified");	    
            http_response_add_header(response, "Content-Type", "application/octet-stream");
            break;
    }
}

static void
raop_handler_fpsetup(raop_conn_t *conn,
                     http_request_t *request, http_response_t *response,
                     char **response_data, int *response_datalen)
{
    const unsigned char *data;
    int datalen;

    data = (unsigned char *) http_request_get_data(request, &datalen);
    if (datalen == 16) {
        *response_data = malloc(142);
        if (*response_data) {
            http_response_add_header(response, "Content-Type", "application/octet-stream");
            if (!fairplay_setup(conn->fairplay, data, (unsigned char *) *response_data)) {
                *response_datalen = 142;
            } else {
                // Handle error?
                free(*response_data);
                *response_data = NULL;
            }
        }
    } else if (datalen == 164) {
        *response_data = malloc(32);
        if (*response_data) {
            http_response_add_header(response, "Content-Type", "application/octet-stream");
            if (!fairplay_handshake(conn->fairplay, data, (unsigned char *) *response_data)) {
                *response_datalen = 32;
            } else {
                // Handle error?
                free(*response_data);
                *response_data = NULL;
            }
        }
    } else {
        logger_log(conn->raop->logger, LOGGER_ERR, "Invalid fp-setup data length");
        return;
    }
}

static void
raop_handler_options(raop_conn_t *conn,
                     http_request_t *request, http_response_t *response,
                     char **response_data, int *response_datalen)
{
    http_response_add_header(response, "Public", "SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");
}

static void
raop_handler_setup(raop_conn_t *conn,
                   http_request_t *request, http_response_t *response,
                   char **response_data, int *response_datalen)
{
    const char *dacp_id;
    const char *active_remote_header;
    bool logger_debug = (logger_get_level(conn->raop->logger) >= LOGGER_DEBUG);
    
    const char *data;
    int data_len;
    data = http_request_get_data(request, &data_len);

    dacp_id = http_request_get_header(request, "DACP-ID");
    active_remote_header = http_request_get_header(request, "Active-Remote");

    if (dacp_id && active_remote_header) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "DACP-ID: %s", dacp_id);
        logger_log(conn->raop->logger, LOGGER_DEBUG, "Active-Remote: %s", active_remote_header);
        if (conn->raop_rtp) {
            raop_rtp_remote_control_id(conn->raop_rtp, dacp_id, active_remote_header);
        }
    }

    // Parsing bplist
    plist_t req_root_node = NULL;
    plist_from_bin(data, data_len, &req_root_node);
    plist_t req_ekey_node = plist_dict_get_item(req_root_node, "ekey");
    plist_t req_eiv_node = plist_dict_get_item(req_root_node, "eiv");
	
    // For the response
    plist_t res_root_node = plist_new_dict();

    if (PLIST_IS_DATA(req_eiv_node) && PLIST_IS_DATA(req_ekey_node)) {
        // The first SETUP call that initializes keys and timing

        unsigned char aesiv[16];
        unsigned char aeskey[16];
        unsigned char eaeskey[72];

        logger_log(conn->raop->logger, LOGGER_DEBUG, "SETUP 1");

        // First setup
        char* eiv = NULL;
        uint64_t eiv_len = 0;

        char *deviceID = NULL;
        char *model = NULL;
        char *name = NULL;
        bool admit_client = true;
        plist_t req_deviceid_node = plist_dict_get_item(req_root_node, "deviceID");
        plist_get_string_val(req_deviceid_node, &deviceID);  
        plist_t req_model_node = plist_dict_get_item(req_root_node, "model");
        plist_get_string_val(req_model_node, &model);  
        plist_t req_name_node = plist_dict_get_item(req_root_node, "name");
        plist_get_string_val(req_name_node, &name);  
	if (conn->raop->callbacks.report_client_request) {
            conn->raop->callbacks.report_client_request(conn->raop->callbacks.cls, deviceID, model, name, &admit_client);
        }
	if (admit_client && deviceID && name && conn->raop->callbacks.register_client) {
            bool pending_registration;
            char *client_device_id;
            char *client_pk;   /* encoded as null-terminated  base64 string*/
            access_client_session_data(conn->session, &client_device_id, &client_pk, &pending_registration);
            if (pending_registration) {
                if (client_pk && !strcmp(deviceID, client_device_id)) { 
                    conn->raop->callbacks.register_client(conn->raop->callbacks.cls, client_device_id, client_pk, name); 
		}
	    }
            if (client_pk) {
                free (client_pk);
            }
	}
        if (deviceID) {
            free (deviceID);
            deviceID = NULL;
        }
        if (model) {
            free (model);
            model = NULL;
        }
        if (name) {
            free (name);
            name = NULL;
        }
        if (admit_client == false) {
            /* client is not authorized to connect */
            plist_free(res_root_node);
            plist_free(req_root_node);
            return;
	}

        plist_get_data_val(req_eiv_node, &eiv, &eiv_len);
        memcpy(aesiv, eiv, 16);
        free(eiv);	
        logger_log(conn->raop->logger, LOGGER_DEBUG, "eiv_len = %llu", eiv_len);
	if (logger_debug) {
            char* str = utils_data_to_string(aesiv, 16, 16);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "16 byte aesiv (needed for AES-CBC audio decryption iv):\n%s", str);
            free(str);
	}

        char* ekey = NULL;
        uint64_t ekey_len = 0;
        plist_get_data_val(req_ekey_node, &ekey, &ekey_len);
        memcpy(eaeskey,ekey,72);
        free(ekey);
        logger_log(conn->raop->logger, LOGGER_DEBUG, "ekey_len = %llu", ekey_len);
        // eaeskey is 72 bytes, aeskey is 16 bytes
	if (logger_debug) {
            char *str = utils_data_to_string((unsigned char *) eaeskey, ekey_len, 16);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "ekey:\n%s", str);
            free (str);
        }
        int ret = fairplay_decrypt(conn->fairplay, (unsigned char*) eaeskey, aeskey);
        logger_log(conn->raop->logger, LOGGER_DEBUG, "fairplay_decrypt ret = %d", ret);
        if (logger_debug) {
            char *str = utils_data_to_string(aeskey, 16, 16);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "16 byte aeskey (fairplay-decrypted from ekey):\n%s", str);
            free(str);
        }

        const char *user_agent = http_request_get_header(request, "User-Agent");
        logger_log(conn->raop->logger, LOGGER_INFO, "Client identified as User-Agent: %s", user_agent);	

        bool old_protocol = false;
#ifdef OLD_PROTOCOL_CLIENT_USER_AGENT_LIST    /* set in global.h */
        if (strstr(OLD_PROTOCOL_CLIENT_USER_AGENT_LIST, user_agent)) old_protocol = true;
#endif
        if  (old_protocol) {    /* some windows AirPlay-client emulators use old AirPlay 1 protocol with unhashed AES key */
            logger_log(conn->raop->logger, LOGGER_INFO, "Client identifed as using old protocol (unhashed) AES audio key)");
        } else {
            unsigned char ecdh_secret[X25519_KEY_SIZE];
            if (pairing_get_ecdh_secret_key(conn->session, ecdh_secret)) {
                /* In this case  (legacy) pairing with client was successfully set up and created the shared ecdh_secret:
                 * aeskey must now be hashed with it
                 *
                 * If byte 27 of features ("supports legacy pairing") is turned off, the client does not request pairsetup
                 * and does NOT set up pairing (this eliminates a 5 second delay in connecting with no apparent bad effects).
                 * In this case, ecdh_secret does not exist, so aeskey should NOT be hashed with it.

                 * UxPlay may be able to function with byte 27 turned off because it currently does not support connections 
                 * with more than one client at a time. AppleTV supports up to 12 clients, uses pairing to give each a distinct
                 * SessionID and ecdh_secret.
            
                 * The "old protocol" Windows AirPlay client AirMyPC seems not to respect the byte 27 setting, and always sets
                 * up the  ecdh_secret, but decryption fails if aeskey is hashed.*/

                if (logger_debug) {
                    char *str = utils_data_to_string(ecdh_secret, X25519_KEY_SIZE, 16);
                    logger_log(conn->raop->logger, LOGGER_DEBUG, "32 byte shared ecdh_secret:\n%s", str);
                    free(str);
                }
                memcpy(eaeskey, aeskey, 16);
                sha_ctx_t *ctx = sha_init();
                sha_update(ctx, eaeskey, 16);
                sha_update(ctx, ecdh_secret, 32);
                sha_final(ctx, eaeskey, NULL);
                sha_destroy(ctx);
                memcpy(aeskey, eaeskey, 16);
                if (logger_debug) {
                    char *str = utils_data_to_string(aeskey, 16, 16);
                    logger_log(conn->raop->logger, LOGGER_DEBUG, "16 byte aeskey after sha-256 hash with ecdh_secret:\n%s", str);
                    free(str);
                }
            }
        }

        // Time port
        plist_t req_is_remote_control_only_node = plist_dict_get_item(req_root_node, "isRemoteControlOnly");
        if (req_is_remote_control_only_node) {
	    uint8_t bool_val = 0;
	    plist_get_bool_val(req_is_remote_control_only_node, &bool_val);  
            if (bool_val) {
                logger_log(conn->raop->logger, LOGGER_ERR, "Client specified AirPlay2 \"Remote Control\" protocol\n"
			   " Only AirPlay v1 protocol (using NTP and timing port) is supported");
            }
	}
        char *timing_protocol = NULL;
	timing_protocol_t time_protocol;
        plist_t req_timing_protocol_node = plist_dict_get_item(req_root_node, "timingProtocol");
        plist_get_string_val(req_timing_protocol_node, &timing_protocol);
        if (timing_protocol) {
             int string_len = strlen(timing_protocol);
             if (strncmp(timing_protocol, "NTP", string_len) == 0) {
                 time_protocol = NTP;
             } else if (strncmp(timing_protocol, "None", string_len) == 0) {
                 time_protocol = TP_NONE;
             } else {
                 time_protocol = TP_OTHER;
             }
             if (time_protocol != NTP) {
                 logger_log(conn->raop->logger, LOGGER_ERR, "Client specified timingProtocol=%s,"
                            " but timingProtocol= NTP is required here", timing_protocol);
             }
             free (timing_protocol);
             timing_protocol = NULL;
        } else {
            logger_log(conn->raop->logger, LOGGER_DEBUG, "Client did not specify timingProtocol,"
                       " old protocol without offset will be used");
            time_protocol = TP_UNSPECIFIED;
        }
        uint64_t timing_rport = 0;
        plist_t req_timing_port_node = plist_dict_get_item(req_root_node, "timingPort");
	if (req_timing_port_node) {
             plist_get_uint_val(req_timing_port_node, &timing_rport);
        }
        if (timing_rport) {
            logger_log(conn->raop->logger, LOGGER_DEBUG, "timing_rport = %llu", timing_rport);
        } else {
            logger_log(conn->raop->logger, LOGGER_ERR, "Client did not supply timing_rport,"
                       " may be using unsupported AirPlay2 \"Remote Control\" protocol");
        }
        unsigned short timing_lport = conn->raop->timing_lport;

        conn->raop_ntp = NULL;
        conn->raop_rtp = NULL;
        conn->raop_rtp_mirror = NULL;
        char remote[40];
        int len = utils_ipaddress_to_string(conn->remotelen, conn->remote, conn->zone_id, remote, (int) sizeof(remote));
        if (!len || len > sizeof(remote)) {
            char *str = utils_data_to_string(conn->remote, conn->remotelen, 16);
            logger_log(conn->raop->logger, LOGGER_ERR, "failed to extract valid client ip address:\n"
                       "*** UxPlay will be unable to send communications to client.\n"
                       "*** address length %d, zone_id %u address data:\n%sparser returned \"%s\"\n",
                       conn->remotelen, conn->zone_id, str, remote);
            free(str);
        }
        conn->raop_ntp = raop_ntp_init(conn->raop->logger, &conn->raop->callbacks, remote,
                                       conn->remotelen, (unsigned short) timing_rport, &time_protocol);
        raop_ntp_start(conn->raop_ntp, &timing_lport, conn->raop->max_ntp_timeouts);
        conn->raop_rtp = raop_rtp_init(conn->raop->logger, &conn->raop->callbacks, conn->raop_ntp,
                                       remote, conn->remotelen, aeskey, aesiv);
        conn->raop_rtp_mirror = raop_rtp_mirror_init(conn->raop->logger, &conn->raop->callbacks,
                                                     conn->raop_ntp, remote, conn->remotelen, aeskey);

        /* the event port is not used in mirror mode or audio mode */
        unsigned short event_port = 0;
        plist_t res_event_port_node = plist_new_uint(event_port);
        plist_t res_timing_port_node = plist_new_uint(timing_lport);
        plist_dict_set_item(res_root_node, "timingPort", res_timing_port_node);
        plist_dict_set_item(res_root_node, "eventPort", res_event_port_node);

        logger_log(conn->raop->logger, LOGGER_DEBUG, "eport = %d, tport = %d", event_port, timing_lport);
    }

    // Process stream setup requests
    plist_t req_streams_node = plist_dict_get_item(req_root_node, "streams");
    if (PLIST_IS_ARRAY(req_streams_node)) {
        plist_t res_streams_node = plist_new_array();

        int count = plist_array_get_size(req_streams_node);
        for (int i = 0; i < count; i++) {
            plist_t req_stream_node = plist_array_get_item(req_streams_node, i);
            plist_t req_stream_type_node = plist_dict_get_item(req_stream_node, "type");
            uint64_t type;
            plist_get_uint_val(req_stream_type_node, &type);
            logger_log(conn->raop->logger, LOGGER_DEBUG, "type = %llu", type);

            switch (type) {
                case 110: {
                    // Mirroring
                    unsigned short dport = conn->raop->mirror_data_lport;
                    plist_t stream_id_node = plist_dict_get_item(req_stream_node, "streamConnectionID");
                    uint64_t stream_connection_id;
                    plist_get_uint_val(stream_id_node, &stream_connection_id);
                    logger_log(conn->raop->logger, LOGGER_DEBUG, "streamConnectionID (needed for AES-CTR video decryption"
                               " key and iv): %llu", stream_connection_id);

                    if (conn->raop_rtp_mirror) {
                        raop_rtp_mirror_init_aes(conn->raop_rtp_mirror, &stream_connection_id);
                        raop_rtp_mirror_start(conn->raop_rtp_mirror, &dport, conn->raop->clientFPSdata);
                        logger_log(conn->raop->logger, LOGGER_DEBUG, "Mirroring initialized successfully");
                    } else {
                        logger_log(conn->raop->logger, LOGGER_ERR, "Mirroring not initialized at SETUP, playing will fail!");
                        http_response_set_disconnect(response, 1);
                    }

                    plist_t res_stream_node = plist_new_dict();
                    plist_t res_stream_data_port_node = plist_new_uint(dport);
                    plist_t res_stream_type_node = plist_new_uint(110);
                    plist_dict_set_item(res_stream_node, "dataPort", res_stream_data_port_node);
                    plist_dict_set_item(res_stream_node, "type", res_stream_type_node);
                    plist_array_append_item(res_streams_node, res_stream_node);

                    break;
                } case 96: {
                    // Audio
                    unsigned short cport = conn->raop->control_lport, dport = conn->raop->data_lport; 
                    unsigned short remote_cport = 0;
                    unsigned char ct;
                    unsigned int sr = AUDIO_SAMPLE_RATE; /* all AirPlay audio formats supported so far have sample rate 44.1kHz */

                    uint64_t uint_val = 0;
                    plist_t req_stream_control_port_node = plist_dict_get_item(req_stream_node, "controlPort");
                    plist_get_uint_val(req_stream_control_port_node, &uint_val);
                    remote_cport = (unsigned short) uint_val;   /* must != 0 to activate audio resend requests */

                    plist_t req_stream_ct_node = plist_dict_get_item(req_stream_node, "ct");
                    plist_get_uint_val(req_stream_ct_node, &uint_val);
                    ct = (unsigned char) uint_val;

                    if (conn->raop->callbacks.audio_get_format) {
		        /* get additional audio format parameters  */
                        uint64_t audioFormat;
                        unsigned short spf;
                        bool isMedia; 
                        bool usingScreen;
                        uint8_t bool_val = 0;

                        plist_t req_stream_spf_node = plist_dict_get_item(req_stream_node, "spf");
                        plist_get_uint_val(req_stream_spf_node, &uint_val);
                        spf = (unsigned short) uint_val;

                        plist_t req_stream_audio_format_node = plist_dict_get_item(req_stream_node, "audioFormat");
                        plist_get_uint_val(req_stream_audio_format_node, &audioFormat);

                        plist_t req_stream_is_media_node = plist_dict_get_item(req_stream_node, "isMedia");
                        if (req_stream_is_media_node) {
                            plist_get_bool_val(req_stream_is_media_node, &bool_val);
                            isMedia = (bool) bool_val;
                        } else {
                            isMedia = false;
                        }

                        plist_t req_stream_using_screen_node = plist_dict_get_item(req_stream_node, "usingScreen");
                        if (req_stream_using_screen_node) {
                            plist_get_bool_val(req_stream_using_screen_node, &bool_val);
                            usingScreen = (bool) bool_val;
                        } else {
                            usingScreen = false;
                        }

                        conn->raop->callbacks.audio_get_format(conn->raop->callbacks.cls, &ct, &spf, &usingScreen, &isMedia, &audioFormat);
                    }

                    if (conn->raop_rtp) {
                        raop_rtp_start_audio(conn->raop_rtp, &remote_cport, &cport, &dport, &ct, &sr);
                        logger_log(conn->raop->logger, LOGGER_DEBUG, "RAOP initialized success");
                    } else {
                        logger_log(conn->raop->logger, LOGGER_ERR, "RAOP not initialized at SETUP, playing will fail!");
                        http_response_set_disconnect(response, 1);
                    }

                    plist_t res_stream_node = plist_new_dict();
                    plist_t res_stream_data_port_node = plist_new_uint(dport);
                    plist_t res_stream_control_port_node = plist_new_uint(cport);
                    plist_t res_stream_type_node = plist_new_uint(96);
                    plist_dict_set_item(res_stream_node, "dataPort", res_stream_data_port_node);
                    plist_dict_set_item(res_stream_node, "controlPort", res_stream_control_port_node);
                    plist_dict_set_item(res_stream_node, "type", res_stream_type_node);
                    plist_array_append_item(res_streams_node, res_stream_node);

                    break;
                }

                default:
                    logger_log(conn->raop->logger, LOGGER_ERR, "SETUP tries to setup stream of unknown type %llu", type);
                    http_response_set_disconnect(response, 1);
                    break;
            }
        }

        plist_dict_set_item(res_root_node, "streams", res_streams_node);
    }

    plist_to_bin(res_root_node, response_data, (uint32_t*) response_datalen);
    plist_free(res_root_node);
    plist_free(req_root_node);
    http_response_add_header(response, "Content-Type", "application/x-apple-binary-plist");
}

static void
raop_handler_get_parameter(raop_conn_t *conn,
                           http_request_t *request, http_response_t *response,
                           char **response_data, int *response_datalen)
{
    const char *content_type;
    const char *data;
    int datalen;

    content_type = http_request_get_header(request, "Content-Type");
    data = http_request_get_data(request, &datalen);
    if (!strcmp(content_type, "text/parameters")) {
        const char *current = data;

        while (current && (datalen - (current - data) > 0)) {
            const char *next;

            /* This is a bit ugly, but seems to be how airport works too */
            if ((datalen - (current - data) >= 8) && !strncmp(current, "volume\r\n", 8)) {
                const char volume[] = "volume: 0.0\r\n";

                http_response_add_header(response, "Content-Type", "text/parameters");
                *response_data = strdup(volume);
                if (*response_data) {
                    *response_datalen = strlen(*response_data);
                }
                return;
            }

            for (next = current ; (datalen - (next - data) > 0) ; ++next)
                if (*next == '\r')
                    break;

            if ((datalen - (next - data) >= 2) && !strncmp(next, "\r\n", 2)) {
                if ((next - current) > 0) {
                    logger_log(conn->raop->logger, LOGGER_WARNING,
                               "Found an unknown parameter: %.*s", (next - current), current);
                }
                current = next + 2;
            } else {
                current = NULL;
            }
        }
    }
}

static void
raop_handler_set_parameter(raop_conn_t *conn,
                           http_request_t *request, http_response_t *response,
                           char **response_data, int *response_datalen)
{
    const char *content_type;
    const char *data;
    int datalen;

    content_type = http_request_get_header(request, "Content-Type");
    data = http_request_get_data(request, &datalen);
    if (!strcmp(content_type, "text/parameters")) {
        char *datastr;
        datastr = calloc(1, datalen+1);
        if (data && datastr && conn->raop_rtp) {
            memcpy(datastr, data, datalen);
            if ((datalen >= 8) && !strncmp(datastr, "volume: ", 8)) {
                float vol = 0.0;
                sscanf(datastr+8, "%f", &vol);
                raop_rtp_set_volume(conn->raop_rtp, vol);
            } else if ((datalen >= 10) && !strncmp(datastr, "progress: ", 10)) {
                unsigned int start, curr, end;
                sscanf(datastr+10, "%u/%u/%u", &start, &curr, &end);
                raop_rtp_set_progress(conn->raop_rtp, start, curr, end);
            }
        } else if (!conn->raop_rtp) {
            logger_log(conn->raop->logger, LOGGER_WARNING, "RAOP not initialized at SET_PARAMETER");
        }
        free(datastr);
    } else if (!strcmp(content_type, "image/jpeg") || !strcmp(content_type, "image/png")) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "Got image data of %d bytes", datalen);
        if (conn->raop_rtp) {
            raop_rtp_set_coverart(conn->raop_rtp, data, datalen);
        } else {
            logger_log(conn->raop->logger, LOGGER_WARNING, "RAOP not initialized at SET_PARAMETER coverart");
        }
    } else if (!strcmp(content_type, "application/x-dmap-tagged")) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "Got metadata of %d bytes", datalen);
        if (conn->raop_rtp) {
            raop_rtp_set_metadata(conn->raop_rtp, data, datalen);
        } else {
            logger_log(conn->raop->logger, LOGGER_WARNING, "RAOP not initialized at SET_PARAMETER metadata");
        }
    }
}


static void
raop_handler_feedback(raop_conn_t *conn,
                      http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen)
{
    logger_log(conn->raop->logger, LOGGER_DEBUG, "raop_handler_feedback");
}

static void
raop_handler_record(raop_conn_t *conn,
                    http_request_t *request, http_response_t *response,
                    char **response_data, int *response_datalen)
{
    char audio_latency[12];
    unsigned int ad = (unsigned int) (((uint64_t) conn->raop->audio_delay_micros) * AUDIO_SAMPLE_RATE / SECOND_IN_USECS);
    snprintf(audio_latency, sizeof(audio_latency), "%u", ad);
    logger_log(conn->raop->logger, LOGGER_DEBUG, "raop_handler_record");
    http_response_add_header(response, "Audio-Latency", audio_latency);
    http_response_add_header(response, "Audio-Jack-Status", "connected; type=analog");
}

static void
raop_handler_flush(raop_conn_t *conn,
                      http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen)
{
    const char *rtpinfo;
    int next_seq = -1;

    rtpinfo = http_request_get_header(request, "RTP-Info");
    if (rtpinfo) {
        logger_log(conn->raop->logger, LOGGER_DEBUG, "Flush with RTP-Info: %s", rtpinfo);
        if (!strncmp(rtpinfo, "seq=", 4)) {
            next_seq = strtol(rtpinfo + 4, NULL, 10);
        }
    }
    if (conn->raop_rtp) {
        raop_rtp_flush(conn->raop_rtp, next_seq);
    } else {
        logger_log(conn->raop->logger, LOGGER_WARNING, "RAOP not initialized at FLUSH");
    }
}

static void
raop_handler_teardown(raop_conn_t *conn,
                      http_request_t *request, http_response_t *response,
                      char **response_data, int *response_datalen)
{
    /* get the teardown request type(s):  (type 96, 110, or none) */
    const char *data;
    int data_len;
    bool teardown_96 = false, teardown_110 = false;
    data = http_request_get_data(request, &data_len);
    plist_t req_root_node = NULL;
    plist_from_bin(data, data_len, &req_root_node);
    char * plist_xml;
    uint32_t plist_len;
    plist_to_xml(req_root_node, &plist_xml, &plist_len);
    logger_log(conn->raop->logger, LOGGER_DEBUG, "%s", plist_xml);
    free(plist_xml);
    plist_t req_streams_node = plist_dict_get_item(req_root_node, "streams");
    /* Process stream teardown requests */
    if (PLIST_IS_ARRAY(req_streams_node)) {
        uint64_t val;
        int count = plist_array_get_size(req_streams_node);
        for (int i = 0; i < count; i++) {
            plist_t req_stream_node = plist_array_get_item(req_streams_node,0);
            plist_t req_stream_type_node = plist_dict_get_item(req_stream_node, "type");
            plist_get_uint_val(req_stream_type_node, &val);
            if (val == 96) {
                teardown_96 = true;
            } else if (val == 110) { 
	        teardown_110 = true;
            }
        }
    }
    plist_free(req_root_node);
    if (conn->raop->callbacks.conn_teardown) {
        conn->raop->callbacks.conn_teardown(conn->raop->callbacks.cls, &teardown_96, &teardown_110);
    }
    logger_log(conn->raop->logger, LOGGER_DEBUG, "TEARDOWN request,  96=%d, 110=%d", teardown_96, teardown_110);
  
    http_response_add_header(response, "Connection", "close");
  
    if (teardown_96) {
        if (conn->raop_rtp) {
            /* Stop our audio RTP session */
            raop_rtp_stop(conn->raop_rtp);
        }
    } else if (teardown_110) {
        if (conn->raop_rtp_mirror) {
        /* Stop our video RTP session */
            raop_rtp_mirror_stop(conn->raop_rtp_mirror);
        }
    } else {
        /* Destroy our sessions */
        if (conn->raop_rtp) {
            raop_rtp_destroy(conn->raop_rtp);
            conn->raop_rtp = NULL;
        }
        if (conn->raop_rtp_mirror) {
            raop_rtp_mirror_destroy(conn->raop_rtp_mirror);
            conn->raop_rtp_mirror = NULL;
        }
    }	
}
