/*
 * librdkafka - The Apache Kafka C/C++ library
 *
 * Copyright (c) 2017 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * Builtin SASL AWS MSK IAM support
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <curl/curl.h>
#include <sys/time.h>
#include <libxml/parser.h>

#include "rdkafka_int.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_sasl.h"
#include "rdkafka_sasl_int.h"

#include "rdstringbuilder.h"
#include "rdtypes.h"
#include "rdunittest.h"

#if WITH_SSL
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include "rdkafka_aws.h"
#else
#error "WITH_SSL (OpenSSL) is required for SASL AWS MSK IAM"
#endif


/**
 * @struct Per-client-instance SASL/AWS_MSK_IAM handle.
 */
typedef struct rd_kafka_sasl_aws_msk_iam_handle_s {
        /**< Read-write lock for fields in the handle. */
        rwlock_t lock;

        /**< Required AWS credential values. */
        char *aws_access_key_id;  /* AWS access key id from conf */
        char *aws_secret_access_key;  /* AWS secret access key from conf */
        char *aws_region;  /* AWS region from conf */
        char *aws_security_token;  /* AWS security token from conf (optional) */

        /**< Error message for validation and/or credential retrieval problems. */
        char *errstr;

        /**< Back-pointer to client instance. */
        rd_kafka_t *rk;

        /**< Credential refresh timer. This will be expiring every 10s if 
         * credential fetches are failing, or 80% of the credential's 
         * lifetime if credential fetches are succeeding.
         */
        rd_kafka_timer_t credential_refresh_tmr;

} rd_kafka_sasl_aws_msk_iam_handle_t;

/**
 * @brief Per-connection state
 */
struct rd_kafka_sasl_aws_msk_iam_state {
        enum {
            RD_KAFKA_SASL_AWS_MSK_IAM_SEND_CLIENT_FIRST_MESSAGE,
            RD_KAFKA_SASL_AWS_MSK_IAM_RECEIVE_SERVER_RESPONSE,
        } state;
        const EVP_MD *md;  /* hash function pointer */
        char *hostname;  /* hostname from client_new */

        /*
         * A place to store a consistent view of the token and extensions
         * throughout the authentication process -- even if it is refreshed
         * midway through this particular authentication.
         */
        char *aws_access_key_id;  /* AWS access key id from conf */
        char *aws_secret_access_key;  /* AWS secret access key from conf */
        char *aws_region;  /* AWS region from conf */
        char *aws_security_token;  /* AWS security token from conf (optional) */
};

/**
 * @brief free memory inside the given credential
 */
static void rd_kafka_sasl_aws_msk_iam_credential_free (
        rd_kafka_aws_credential_t *credential) {
        RD_IF_FREE(credential->aws_access_key_id, rd_free);
        RD_IF_FREE(credential->aws_secret_access_key, rd_free);
        RD_IF_FREE(credential->aws_region, rd_free);
        RD_IF_FREE(credential->aws_security_token, rd_free);

        memset(credential, 0, sizeof(*credential));
}

static void
rd_kafka_sasl_aws_msk_iam_credential_refresh_tmr_cb(rd_kafka_timers_t *rkts,
                                                    void *arg);

/**
 * @brief Set SASL/AWS_MSK_IAM token and metadata
 *
 * @param rk Client instance.
 * @param aws_access_key_id Access key id.
 * @param aws_secret_access_key Secret access key.
 * @param aws_region AWS region used in signing and for STS endpoint.
 * @param aws_security_token Temporary AWS security token. Required for using STS.
 *  Use rd_kafka_sasl_aws_msk_iam_credential_free() to free members if
 *  return value is not -1.
 * @param md_lifetime_ms when the credential expires, in terms of the number of
 *  milliseconds since the epoch. See https://currentmillis.com/.
 *
 * @returns \c RD_KAFKA_RESP_ERR_NO_ERROR on success, otherwise errstr set and:
 *          \c RD_KAFKA_RESP_ERR__INVALID_ARG if any of the arguments are
 *              invalid;
 *          \c RD_KAFKA_RESP_ERR__STATE if SASL/OAUTHBEARER is not configured as
 *              the client's authentication mechanism.
 *
 * @sa rd_kafka_aws_msk_iam_set_credential_failure
 */
static rd_kafka_resp_err_t
rd_kafka_aws_msk_iam_set_credential (rd_kafka_t *rk,
        const char *aws_access_key_id,
        const char *aws_secret_access_key,
        const char *aws_region,
        const char *aws_security_token,
        int64_t md_lifetime_ms,
        char *errstr, size_t errstr_size) {
        rd_kafka_sasl_aws_msk_iam_handle_t *handle = rk->rk_sasl.handle;
        rd_ts_t now_wallclock;
        rd_ts_t wts_md_lifetime = md_lifetime_ms * 1000;

        /* Check if SASL/AWS_MSK_IAM is the configured auth mechanism */
        if (rk->rk_conf.sasl.provider != &rd_kafka_sasl_aws_msk_iam_provider ||
            !handle) {
                rd_snprintf(errstr, errstr_size, "SASL/AWS_MSK_IAM is not the "
                            "configured authentication mechanism");
                return RD_KAFKA_RESP_ERR__STATE;
        }

        /* Check args for correct format/value */
        now_wallclock = rd_uclock();
        if (wts_md_lifetime <= now_wallclock) {
                rd_snprintf(errstr, errstr_size,
                            "Must supply an unexpired token: "
                            "now=%"PRId64"ms, exp=%"PRId64"ms",
                            now_wallclock/1000, wts_md_lifetime/1000);
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }

        rwlock_wrlock(&handle->lock);

        RD_IF_FREE(handle->aws_access_key_id, rd_free);
        handle->aws_access_key_id = rd_strdup(aws_access_key_id);

        RD_IF_FREE(handle->aws_secret_access_key, rd_free);
        handle->aws_secret_access_key = rd_strdup(aws_secret_access_key);

        RD_IF_FREE(handle->aws_region, rd_free);
        handle->aws_region = rd_strdup(aws_region);

        RD_IF_FREE(handle->aws_security_token, rd_free);
        if (aws_security_token) {
                handle->aws_security_token = rd_strdup(aws_security_token);
        }
        else handle->aws_security_token = NULL;

        /* Schedule a refresh 80% through its remaining lifetime */
        rd_ts_t interval = ((wts_md_lifetime - now_wallclock) * 8ul) / 10ul;
        rd_kafka_timer_start(&rk->rk_timers,
                &handle->credential_refresh_tmr, interval,
                rd_kafka_sasl_aws_msk_iam_credential_refresh_tmr_cb, rk);

        RD_IF_FREE(handle->errstr, rd_free);
        handle->errstr = NULL;

        rwlock_wrunlock(&handle->lock);

        rd_kafka_dbg(rk, SECURITY, "BRKMAIN",
                     "Waking up waiting broker threads after "
                     "setting AWS_MSK_IAM credential");
        rd_kafka_all_brokers_wakeup(rk, RD_KAFKA_BROKER_STATE_TRY_CONNECT,
                                        "AWS_MSK_IAM credential refresh");

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief SASL/AWS_MSK_IAM credential refresh failure indicator.
 *
 * @param rk Client instance.
 * @param errstr mandatory human readable error reason for failing to acquire
 *  a credential.
 *
 */
static void
rd_kafka_aws_msk_iam_set_credential_failure(rd_kafka_t *rk, const char *errstr) {
        rd_kafka_sasl_aws_msk_iam_handle_t *handle = rk->rk_sasl.handle;
        rd_bool_t error_changed;

        /* Check if SASL/AWS_MSK_IAM is the configured auth mechanism */
        if (rk->rk_conf.sasl.provider != &rd_kafka_sasl_aws_msk_iam_provider ||
            !handle) {
                return;
        }

        rwlock_wrlock(&handle->lock);

        /* Leave any existing credential because it may have some life left,
         * schedule a refresh for 10 seconds later. */
        rd_kafka_timer_start(&rk->rk_timers,
                &handle->credential_refresh_tmr, 10l * 1000 * 1000,
                rd_kafka_sasl_aws_msk_iam_credential_refresh_tmr_cb, rk);

        if (!errstr || !*errstr) {
                rwlock_wrunlock(&handle->lock);
                return;
        }
        
        error_changed = !handle->errstr ||
                strcmp(handle->errstr, errstr);
        RD_IF_FREE(handle->errstr, rd_free);
        handle->errstr = rd_strdup(errstr);
        rwlock_wrunlock(&handle->lock);

        /* Trigger an ERR__AUTHENTICATION error if the error changed. */
        if (error_changed) {
                rd_kafka_op_err(rk, RD_KAFKA_RESP_ERR__AUTHENTICATION,
                                "Failed to acquire SASL AWS_MSK_IAM credential: %s",
                                errstr);
        }
}

static int
rd_kafka_aws_msk_iam_credential_refresh0(
                                rd_kafka_t *rk,
                                rd_kafka_aws_credential_t *credential,
                                char *errstr, size_t errstr_size) {
        const rd_kafka_conf_t *conf = &rk->rk_conf;
        rd_kafka_sasl_aws_msk_iam_handle_t *handle = rk->rk_sasl.handle;
        int ret = RD_KAFKA_RESP_ERR_UNKNOWN;
        
        str_builder_t *sb;
        sb = str_builder_create();
        
        memset(credential, 0, sizeof(*credential));

        time_t t = time(&t);
        struct tm *tmp = gmtime(&t);  // must use UTC time
        char *ymd = rd_malloc(sizeof(char) * 9);
        char *hms = rd_malloc(sizeof(char) * 7);
        strftime(ymd, sizeof(char) * 9, "%Y%m%d", tmp);
        strftime(hms, sizeof(char) * 7, "%H%M%S", tmp);

        rwlock_wrlock(&handle->lock);
        char* aws_access_key_id = rd_strdup(conf->sasl.aws_access_key_id);
        char* aws_secret_access_key = rd_strdup(conf->sasl.aws_secret_access_key);
        char* aws_region = rd_strdup(conf->sasl.aws_region);

        /* parameters to build request_parameters */
        char* role_arn = rd_kafka_aws_uri_encode(conf->sasl.role_arn);
        char* role_session_name = rd_strdup(conf->sasl.role_session_name);
        char* external_id = NULL;
        if (conf->sasl.aws_external_id) {
                external_id = rd_kafka_aws_uri_encode(
                    conf->sasl.aws_external_id);
        }

        char duration_sec[256];
        rd_snprintf(duration_sec, sizeof(duration_sec), "%d", conf->sasl.duration_sec);
        char *action = "AssumeRole";
        char *version = "2011-06-15";
        /******************************************/
        rwlock_wrunlock(&handle->lock);

        char *host = "sts.amazonaws.com";
        char *aws_service = "sts";
        char *method = "POST";
        char *algorithm = "AWS4-HMAC-SHA256";
        const EVP_MD *md = EVP_get_digestbyname("SHA256");
        char *signed_headers = "content-length;content-type;host;x-amz-date";

        str_builder_add_str(sb, "Action=");
        str_builder_add_str(sb, action);
        str_builder_add_str(sb, "&DurationSeconds=");
        str_builder_add_str(sb, duration_sec);
        str_builder_add_str(sb, "&RoleArn=");
        str_builder_add_str(sb, role_arn);
        str_builder_add_str(sb, "&RoleSessionName=");
        str_builder_add_str(sb, role_session_name);
        if (external_id) {
                str_builder_add_str(sb, "&ExternalId=");
                str_builder_add_str(sb, external_id);
        }
        str_builder_add_str(sb, "&Version=");
        str_builder_add_str(sb, version);
        char *request_parameters = str_builder_dump(sb);
        str_builder_clear(sb);

        char content_length[256];
        rd_snprintf(content_length, sizeof(content_length), "%zu", strlen(request_parameters));
        str_builder_add_str(sb, "content-length:");
        str_builder_add_str(sb, content_length);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, "content-type:application/x-www-form-urlencoded; charset=utf-8");
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, "host:");
        str_builder_add_str(sb, host);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, "x-amz-date:");
        str_builder_add_str(sb, ymd);
        str_builder_add_str(sb, "T");
        str_builder_add_str(sb, hms);
        str_builder_add_str(sb, "Z");
        str_builder_add_str(sb, "\n");
        char *canonical_headers = str_builder_dump(sb);

        str_builder_destroy(sb);

        credential->aws_region = rd_strdup(aws_region);
        credential->md_lifetime_ms
            = rd_uclock() / 1000 + conf->sasl.duration_sec * 1000;
        rd_kafka_dbg(rk, SECURITY, "SASLAWSMSKIAM", "Sending refresh request to STS");
        ret = rd_kafka_aws_send_request(rk,
                                        credential,
                                        ymd,
                                        hms,
                                        host,
                                        aws_access_key_id,
                                        aws_secret_access_key,
                                        aws_region,
                                        aws_service,
                                        method,
                                        algorithm,
                                        canonical_headers,
                                        signed_headers,
                                        request_parameters,
                                        md,
                                        errstr,
                                        errstr_size);
        if (ret == RD_KAFKA_RESP_ERR_NO_ERROR) {
                rd_kafka_dbg(rk, SECURITY, "SASLAWSMSKIAM",
                    "New AWS credentials retrieved from STS");
        }
        else {
                rd_kafka_dbg(rk, SECURITY, "SASLAWSMSKIAM",
                    "AWS credential retrieval and parsing failed: %s",
                    errstr);
        }

        RD_IF_FREE(aws_access_key_id, rd_free);
        RD_IF_FREE(aws_secret_access_key, rd_free);
        RD_IF_FREE(aws_region, rd_free);
        RD_IF_FREE(ymd, rd_free);
        RD_IF_FREE(hms, rd_free);
        RD_IF_FREE(external_id, rd_free);
        RD_IF_FREE(role_session_name, rd_free);
        RD_IF_FREE(role_arn, rd_free);
        RD_IF_FREE(canonical_headers, rd_free);
        RD_IF_FREE(request_parameters, rd_free);

        return ret;
}

/**
 * @brief SASL/AWS_MSK_IAM credential refresher used for retrieving new temporary
 * credentials from AWS STS service. The refresher will make use of the regional STS
 * endpoints as per https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_temp_enable-regions.html.
 * 
 * If STS is not used and permanent credentials are provided, the refresher essentially performs a NOOP
 * and will not update the AWS credential information.
 */
static void
rd_kafka_aws_msk_iam_credential_refresh (rd_kafka_t *rk, void *opaque) {
        char errstr[512] = "";
        rd_kafka_aws_credential_t credential = RD_ZERO_INIT;
        
        rd_kafka_dbg(rk, SECURITY, "SASLAWSMSKIAM", "Checking whether to refresh AWS credentials");

        if (rk->rk_conf.sasl.enable_use_sts) {
                rd_kafka_dbg(rk, SECURITY, "SASLAWSMSKIAM", "Use STS enabled, will refresh credentials");

                if ((rd_kafka_aws_msk_iam_credential_refresh0(
                        rk, &credential, errstr, sizeof(errstr))
                        != RD_KAFKA_RESP_ERR_NO_ERROR)
                    || (rd_kafka_aws_msk_iam_set_credential(
                        rk, credential.aws_access_key_id,
                        credential.aws_secret_access_key, credential.aws_region,
                        credential.aws_security_token, credential.md_lifetime_ms,
                        errstr, sizeof(errstr)) != RD_KAFKA_RESP_ERR_NO_ERROR)) {
                        rd_kafka_aws_msk_iam_set_credential_failure(rk, errstr);
                }
        } else {
                rd_kafka_dbg(rk, SECURITY, "SASLAWSMSKIAM", "Use STS not enabled, will not refresh credentials");
        }
        rd_kafka_sasl_aws_msk_iam_credential_free(&credential);
}

/**
 * @brief Build client first message
 * 
 *        Builds the first message for the payload
 *        by combining canonical request, signature, and credentials.
 *
 * @remark out->ptr is allocated and will need to be freed.
 */
static void 
rd_kafka_sasl_aws_msk_iam_build_client_first_message (
        rd_kafka_transport_t *rktrans, 
        rd_chariov_t *out) {
        struct rd_kafka_sasl_aws_msk_iam_state *state = rktrans->rktrans_sasl.state;
        rd_kafka_t *rk = rktrans->rktrans_rkb->rkb_rk;
        
        char *aws_service = "kafka-cluster";
        char *algorithm = "AWS4-HMAC-SHA256";
        char *signed_headers = "host";
        char *method = "GET";
        char *request_parameters = "";
        char *action = "kafka-cluster:Connect";

        time_t t = time(&t);
        struct tm *tmp = gmtime(&t);  // must use UTC time
        char *ymd = rd_malloc(sizeof(char) * 9);
        char *hms = rd_malloc(sizeof(char) * 7);
        strftime(ymd, sizeof(char) * 9, "%Y%m%d", tmp);
        strftime(hms, sizeof(char) * 7, "%H%M%S", tmp);

        char *canonical_querystring = rd_kafka_aws_build_sasl_canonical_querystring(
                rk,
                action,
                state->aws_access_key_id,
                state->aws_region,
                ymd,
                hms,
                aws_service,
                state->aws_security_token
        );

        str_builder_t *sb;
        sb = str_builder_create();
        str_builder_add_str(sb, "host:");
        str_builder_add_str(sb, state->hostname);
        str_builder_add_str(sb, "\n");
        char *canonical_headers = str_builder_dump(sb);
        str_builder_destroy(sb);

        rd_kafka_dbg(rk, SECURITY, "AWS", "canonical_headers=\"%s\"", 
            canonical_headers);

        char *sasl_payload = rd_kafka_aws_build_sasl_payload(rk,
                                                        ymd,
                                                        hms,
                                                        state->hostname,
                                                        state->aws_access_key_id,
                                                        state->aws_secret_access_key,
                                                        state->aws_security_token,
                                                        state->aws_region,
                                                        aws_service,
                                                        method,
                                                        algorithm,
                                                        canonical_headers,
                                                        canonical_querystring,
                                                        signed_headers,
                                                        request_parameters,
                                                        state->md);
        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY,
                           "SASLAWSMSKIAM",
                           "SASL payload calculated as %s",
                           sasl_payload);
        
        /* Save JSON to out pointer for sending */
        out->size = strlen(sasl_payload);
        out->ptr = rd_malloc(out->size + 1);
        
        rd_snprintf(out->ptr, out->size + 1,
                    "%s", sasl_payload);
        
        RD_IF_FREE(ymd, rd_free);
        RD_IF_FREE(hms, rd_free);
        RD_IF_FREE(canonical_querystring, rd_free);
        RD_IF_FREE(canonical_headers, rd_free);
        RD_IF_FREE(sasl_payload, rd_free);
}

/**
 * @brief Handle server-response
 * 
 *        This is the end of authentication and the AWS MSK IAM state
 *        will be freed at the end of this function regardless of
 *        authentication outcome.
 *
 * @returns -1 on failure
 */
static int
rd_kafka_sasl_aws_msk_iam_handle_server_response (
        rd_kafka_transport_t *rktrans,
        const rd_chariov_t *in,
        char *errstr, size_t errstr_size) {
        if (in->size) {
            rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER, "SASLAWSMSKIAM",
                           "Received non-empty SASL AWS MSK IAM (builtin) "
                           "response from broker (%s)", in->ptr);
            rd_kafka_sasl_auth_done(rktrans);
            return 0;
        } else {
            rd_snprintf(errstr, errstr_size,
                        "SASL AWS MSK IAM authentication failed: "
                        "Broker response: %s", in->ptr);
            return -1;
        }
}

/**
 * @brief SASL AWS MSK IAM client state machine
 * @returns -1 on failure (errstr set), else 0.
 */
static int rd_kafka_sasl_aws_msk_iam_fsm (rd_kafka_transport_t *rktrans,
                                    const rd_chariov_t *in,
                                    char *errstr, size_t errstr_size) {
        static const char *state_names[] = {
                    "client-first-message",
                    "server-response",
        };
        struct rd_kafka_sasl_aws_msk_iam_state *state = rktrans->rktrans_sasl.state;
        rd_chariov_t out = RD_ZERO_INIT;
        int r = -1;
        rd_ts_t ts_start = rd_clock();
        int prev_state = state->state;
        
        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER, "SASLAWSMSKIAM",
                   "SASL AWS MSK IAM client in state %s",
                   state_names[state->state]);
        
        switch (state->state)
        {
        case RD_KAFKA_SASL_AWS_MSK_IAM_SEND_CLIENT_FIRST_MESSAGE:
            rd_assert(!in); /* Not expecting any server-input */
            
            rd_kafka_sasl_aws_msk_iam_build_client_first_message(rktrans, &out);
            state->state = RD_KAFKA_SASL_AWS_MSK_IAM_RECEIVE_SERVER_RESPONSE;
            break;
        case RD_KAFKA_SASL_AWS_MSK_IAM_RECEIVE_SERVER_RESPONSE:
            rd_assert(in);  /* Requires server-input */
            r = rd_kafka_sasl_aws_msk_iam_handle_server_response(
                        rktrans, in, errstr, errstr_size);
            break;
        }
        
        if (out.ptr) {
                r = rd_kafka_sasl_send(rktrans, out.ptr, (int)out.size,
                                       errstr, errstr_size);
                RD_IF_FREE(out.ptr, rd_free);
        }
        
        ts_start = (rd_clock() - ts_start) / 1000;
        if (ts_start >= 100) {
                rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER, "SASLAWSMSKIAM",
                           "SASL AWS MSK IAM state %s handled in %"PRId64"ms",
                           state_names[prev_state], ts_start);
        }
        
        return r;
}

/**
 * @brief Handle received frame from broker.
 */
static int rd_kafka_sasl_aws_msk_iam_recv (rd_kafka_transport_t *rktrans,
                                     const void *buf, size_t size,
                                     char *errstr, size_t errstr_size) {
        const rd_chariov_t in = { .ptr = (char *)buf, .size = size };
        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER,
            "SASLAWSMSKIAM",
            "rd_kafka_sasl_aws_msk_iam_recv: buf=%p size=%zu",
            buf, size);
        return rd_kafka_sasl_aws_msk_iam_fsm(rktrans, &in, errstr, errstr_size);
}

/**
 * @brief Initialize and start SASL AWS MSK IAM (builtin) authentication.
 *
 * Returns 0 on successful init and -1 on error.
 *
 * @locality broker thread
 */
static int rd_kafka_sasl_aws_msk_iam_client_new (rd_kafka_transport_t *rktrans,
                                    const char *hostname,
                                    char *errstr, size_t errstr_size) {
        rd_kafka_t* rk = rktrans->rktrans_rkb->rkb_rk;
        rd_kafka_sasl_aws_msk_iam_handle_t* handle = rk->rk_sasl.handle;
        
        rd_rkb_dbg(rktrans->rktrans_rkb, SECURITY | RD_KAFKA_DBG_BROKER, "SASLAWSMSKIAM",
                   "SASL AWS MSK IAM new client initializing");

        struct rd_kafka_sasl_aws_msk_iam_state* state
            = rd_calloc(1, sizeof(*state));
        state->state = RD_KAFKA_SASL_AWS_MSK_IAM_SEND_CLIENT_FIRST_MESSAGE;

        /*
         * Save off the state structure now, before any possibility of
         * returning, so that we will always free up the allocated memory in
         * rd_kafka_sasl_aws_msk_iam_close().
         */
        rktrans->rktrans_sasl.state = state;

        /*
         * Make sure we have a consistent view of the token and extensions
         * throughout the authentication process -- even if it is refreshed
         * midway through this particular authentication.
         */
        rwlock_rdlock(&handle->lock);
        if (!handle->aws_access_key_id || !handle->aws_secret_access_key
            || !handle->aws_region) {
                rd_snprintf(errstr, errstr_size,
                            "AWS_MSK_IAM cannot log in because there "
                            "is no credentials available; last error: %s",
                            handle->errstr ?
                            handle->errstr : "(not available)");
                rwlock_rdunlock(&handle->lock);
                return -1;
        }
        else if (rk->rk_conf.sasl.enable_use_sts 
            && !handle->aws_security_token) {
                rd_snprintf(errstr, errstr_size,
                            "AWS_MSK_IAM_STS cannot authenticate; last error: %s",
                            handle->errstr ?
                            handle->errstr : "(not available)");
                rwlock_rdunlock(&handle->lock);
                return -1;
        }

        state->md = EVP_get_digestbyname("SHA256");
        state->hostname = rd_strdup(hostname);
        state->aws_access_key_id = rd_strdup(handle->aws_access_key_id);
        state->aws_secret_access_key = rd_strdup(handle->aws_secret_access_key);
        state->aws_region = rd_strdup(handle->aws_region);
        
        if (handle->aws_security_token != NULL) {
            state->aws_security_token = rd_strdup(handle->aws_security_token);
        }

        rwlock_rdunlock(&handle->lock);
        
        /* Kick off the FSM */
        return rd_kafka_sasl_aws_msk_iam_fsm(rktrans, NULL, errstr, errstr_size);
}

/**
 * @brief Credential refresh timer callback.
 *
 * @locality rdkafka main thread
 */
static void
rd_kafka_sasl_aws_msk_iam_credential_refresh_tmr_cb(rd_kafka_timers_t *rkts,
                                                    void *arg) {
        rd_kafka_t *rk = arg;
        rd_kafka_aws_msk_iam_credential_refresh(rk, rk->rk_conf.opaque);
}

/**
 * @brief Per-client-instance initializer
 */
static int rd_kafka_sasl_aws_msk_iam_init (rd_kafka_t *rk,
                                           char *errstr, size_t errstr_size) {
        rd_kafka_sasl_aws_msk_iam_handle_t *handle;
        const rd_kafka_conf_t *conf = &rk->rk_conf;

        handle = rd_calloc(1, sizeof(*handle));
        rk->rk_sasl.handle = handle;

        rwlock_init(&handle->lock);

        handle->rk = rk;


        // Set initial handle creds which will be passed into *state in client_new()
        /* Check if SASL/AWS_MSK_IAM is the configured auth mechanism */
        if (rk->rk_conf.sasl.provider != &rd_kafka_sasl_aws_msk_iam_provider ||
            !handle) {
                rd_snprintf(errstr, errstr_size, "SASL/AWS_MSK_IAM is not the "
                            "configured authentication mechanism");
                return RD_KAFKA_RESP_ERR__STATE;
        }

        rwlock_wrlock(&handle->lock);

        handle->aws_access_key_id = rd_strdup(conf->sasl.aws_access_key_id);
        handle->aws_secret_access_key = rd_strdup(conf->sasl.aws_secret_access_key);
        handle->aws_region = rd_strdup(conf->sasl.aws_region);

        if (conf->sasl.enable_use_sts && conf->sasl.duration_sec > 0) {
                /* Schedule a refresh 80% through its remaining lifetime */
                rd_kafka_timer_start(&rk->rk_timers,
                                     &handle->credential_refresh_tmr,
                                     conf->sasl.duration_sec * 800ul * 1000ul,
                                     rd_kafka_sasl_aws_msk_iam_credential_refresh_tmr_cb,
                                     rk);
                /* Kick off the timer immediately to refresh the credentials.
                 * (Timer is triggered from the main loop). */
                rd_kafka_timer_override_once(&rk->rk_timers,
                                             &handle->credential_refresh_tmr,
                                             0 /*immediately*/);
                rd_kafka_dbg(rk, SECURITY, "SASLAWSMSKIAM",
                             "Enqueuing credential refresh");
        }

        handle->errstr = NULL;

        rwlock_wrunlock(&handle->lock);

        return 0;
}

/**
 * @brief Per-client-instance destructor
 */
static void rd_kafka_sasl_aws_msk_iam_term (rd_kafka_t *rk) {
        rd_kafka_sasl_aws_msk_iam_handle_t *handle = rk->rk_sasl.handle;

        if (!handle) {
                return;
        }

        rk->rk_sasl.handle = NULL;

        rd_kafka_timer_stop(&rk->rk_timers, &handle->credential_refresh_tmr, 1);

        RD_IF_FREE(handle->aws_access_key_id, rd_free);
        RD_IF_FREE(handle->aws_secret_access_key, rd_free);
        RD_IF_FREE(handle->aws_region, rd_free);
        RD_IF_FREE(handle->aws_security_token, rd_free);
        RD_IF_FREE(handle->errstr, rd_free);

        rwlock_destroy(&handle->lock);

        rd_free(handle);
}

/**
 * @brief Close and free authentication state
 */
static void rd_kafka_sasl_aws_msk_iam_close (rd_kafka_transport_t *rktrans) {
        struct rd_kafka_sasl_aws_msk_iam_state *state = 
                rktrans->rktrans_sasl.state;

        if (!state) {
                return;
        }
        
        rktrans->rktrans_sasl.state = NULL;
        
        RD_IF_FREE(state->hostname, rd_free);
        RD_IF_FREE(state->aws_access_key_id, rd_free);
        RD_IF_FREE(state->aws_secret_access_key, rd_free);
        RD_IF_FREE(state->aws_region, rd_free);
        RD_IF_FREE(state->aws_security_token, rd_free);

        rd_free(state);
}

/**
 * @brief Validate AWS MSK IAM config and look up the hash function
 */
static int rd_kafka_sasl_aws_msk_iam_conf_validate (rd_kafka_t *rk,
                                              char *errstr,
                                              size_t errstr_size) {        
        if (!rk->rk_conf.sasl.aws_access_key_id || !rk->rk_conf.sasl.aws_secret_access_key || !rk->rk_conf.sasl.aws_region) {
                rd_snprintf(errstr, errstr_size,
                            "sasl.aws_access_key_id, sasl.aws_secret_access_key, and sasl.aws_region must be set");
                return -1;
        }

        if (rk->rk_conf.sasl.enable_use_sts && 
                (!rk->rk_conf.sasl.role_arn || !rk->rk_conf.sasl.role_session_name)) {
                rd_snprintf(errstr, errstr_size,
                            "sasl.enable_use_sts is true but missing sasl.role_arn or sasl.role_session_name");
                return -1;
        }
        
        return 0;
}

void rd_kafka_sasl_aws_msk_iam_global_term(void) {
        #if 0
                xmlCleanupParser(); 
        #endif
}

int rd_kafka_sasl_aws_msk_iam_global_init(void) {
        static rd_atomic32_t has_been_called;

        if (!rd_atomic32_exchange(&has_been_called, 1)) {
                xmlInitParser();
        }
        
        return 0;
}

const struct rd_kafka_sasl_provider rd_kafka_sasl_aws_msk_iam_provider = {
        .name           = "AWS_MSK_IAM",
        .init           = rd_kafka_sasl_aws_msk_iam_init,
        .term           = rd_kafka_sasl_aws_msk_iam_term,
        .client_new     = rd_kafka_sasl_aws_msk_iam_client_new,
        .recv           = rd_kafka_sasl_aws_msk_iam_recv,
        .close          = rd_kafka_sasl_aws_msk_iam_close,
        .conf_validate  = rd_kafka_sasl_aws_msk_iam_conf_validate,
};

/**
 * @name Unit tests
 */

int unittest_aws_msk_iam (void) {
        int fails = 0;

        return fails;
}
