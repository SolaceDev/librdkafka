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
 * AWS API utilities
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <curl/curl.h>
#include <libxml/parser.h>

#include "rdkafka_int.h"
#include "rdkafka_transport.h"
#include "rdkafka_transport_int.h"
#include "rdkafka_sasl.h"
#include "rdkafka_sasl_int.h"
#include "rdkafka_aws.h"

#include "rdstringbuilder.h"
#include "rdtypes.h"
#include "rdunittest.h"

#if WITH_SSL
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <openssl/ossl_typ.h>
#else
#error "WITH_SSL (OpenSSL) is required for AWS API calls"
#endif

#define CHUNK_SIZE 2048


/**
 * @struct Curl get in-memory buffer.
 */
typedef struct {
    unsigned char *buffer;
    size_t len;
    size_t buflen;
} curl_in_mem_buf;

/**
 * @brief Curl callback to write response contents
 */
static size_t rd_kafka_aws_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    size_t realsize = size * nmemb; 
    curl_in_mem_buf *req = (curl_in_mem_buf *) userdata;

    #if 0
            printf("received chunk of %zu bytes\n", realsize);
    #endif    

    while (req->buflen < req->len + realsize + 1)
    {
        req->buffer = realloc(req->buffer, req->buflen + CHUNK_SIZE);
        req->buflen += CHUNK_SIZE;
    }
    memcpy(&req->buffer[req->len], ptr, realsize);
    req->len += realsize;
    req->buffer[req->len] = 0;

    return realsize;
}

static int rd_kafak_aws_curl_debug_callback(CURL* curl,
                                            curl_infotype type,
                                            char *data,
                                            size_t size,
                                            void *rkv) {
        rd_kafka_t *rk = rkv;
        
        switch (type) {
                case CURLINFO_TEXT:
                        rd_kafka_dbg(rk, SECURITY, "CURLINFO_TEXT", 
                            "%*s", (int) size, data);
                        break;
                case CURLINFO_HEADER_IN:
                        rd_kafka_dbg(rk, SECURITY, "CURLINFO_HEADER_IN", 
                            "%*s", (int) size, data);
                        break;
                case CURLINFO_HEADER_OUT:
                        rd_kafka_dbg(rk, SECURITY, "CURLINFO_HEADER_OUT", 
                            "%*s", (int) size, data);
                        break;
                case CURLINFO_DATA_IN:
                        rd_kafka_dbg(rk, SECURITY, "CURLINFO_DATA_IN", 
                            "%*s", (int) size, data);
                        break;
                case CURLINFO_DATA_OUT:
                        rd_kafka_dbg(rk, SECURITY, "CURLINFO_DATA_OUT", 
                            "%*s", (int) size, data);
                        break;
                
                default: break;
        }
        return 0;
}

/**
 * @brief Uri escapes a string
 * @remark ret string will be allocated and must be freed.
 */
char *rd_kafka_aws_uri_encode (const char *in) {
        char *ret;
        int ret_len;
        
        CURL *curl = curl_easy_init();
        if (curl) {
            char *encoded = curl_easy_escape(curl, in, (int)strlen(in));
            ret_len = strlen(encoded)+1;
            ret = rd_malloc(ret_len);
            memcpy(ret, encoded, ret_len);
            
            curl_free(encoded);
            curl_easy_cleanup(curl);
            return ret;
        }
        
        return NULL;
}

/**
 * @brief HMAC_SHA256 encoding
 */
static unsigned char *rd_kafka_aws_hmac_sha256 (const void *key, int keylen,
                              const unsigned char *data, int datalen,
                              unsigned char *result, unsigned int *resultlen) {
    return HMAC(EVP_sha256(), key, keylen, data, datalen, result, resultlen);
}

/**
 * @brief constructs full date format of yyyymmddTHHMMssZ
 * @remark amz_date_buf will be allocated and must be freed.
 */
static char *rd_kafka_aws_construct_amz_date (const char *ymd, const char *hms) {
        int amz_date_size = strlen(ymd) + strlen(hms) + strlen("TZ") + 1;
        char *amz_date_buf;
        
        amz_date_buf = rd_malloc(amz_date_size);
        rd_snprintf(amz_date_buf, amz_date_size, "%sT%sZ", ymd, hms);
        
        return amz_date_buf;
}

/**
 * @brief constructs authorization_header
 * @remark authorization_header will be allocated and must be freed.
 */
static char *rd_kafka_aws_construct_authorization_header (const char *algorithm, 
                                                const char *aws_access_key_id,
                                                const char *credential_scope,
                                                const char *signed_headers,
                                                const char *signature) {
        str_builder_t *sb;
        sb = str_builder_create();
        str_builder_add_str(sb, algorithm);
        str_builder_add_str(sb, " Credential=");
        str_builder_add_str(sb, aws_access_key_id);
        str_builder_add_str(sb, "/");
        str_builder_add_str(sb, credential_scope);
        str_builder_add_str(sb, ", SignedHeaders=");
        str_builder_add_str(sb, signed_headers);
        str_builder_add_str(sb, ", Signature=");
        str_builder_add_str(sb, signature);
        
        char *authorization_header = str_builder_dump(sb);

        str_builder_destroy(sb);
        
        return authorization_header;
}

/**
 * @brief constructs credential_scope
 * @remark credential_scope will be allocated and must be freed.
 */
static char *rd_kafka_aws_construct_credential_scope (const char *ymd,
                                                const char *aws_region,
                                                const char *aws_service) {
        int credential_scope_size = strlen(ymd) + strlen(aws_region) + strlen(aws_service) + strlen("///aws4_request") + 1;
        char *credential_scope;
        credential_scope = rd_malloc(credential_scope_size);
        rd_snprintf(credential_scope, credential_scope_size, "%s/%s/%s/aws4_request", ymd, aws_region, aws_service);

        return credential_scope;
}

/**
 * @brief Generates a canonical query string
 * @remark canonical_query_string will be allocated and must be freed.
 */
char *rd_kafka_aws_build_sasl_canonical_querystring (rd_kafka_t* rk,
                                                const char *action,
                                                const char *aws_access_key_id,
                                                const char *aws_region,
                                                const char *ymd,
                                                const char *hms,
                                                const char *aws_service,
                                                const char *aws_security_token) {
        char *uri_action = rd_kafka_aws_uri_encode(action);
        
        char *credential_scope = rd_kafka_aws_construct_credential_scope(
                ymd,
                aws_region,
                aws_service
        );
        str_builder_t *sb;
        sb = str_builder_create();
        str_builder_add_str(sb, aws_access_key_id);
        str_builder_add_str(sb, "/");
        str_builder_add_str(sb, credential_scope);
        char *credential = str_builder_dump(sb);
        str_builder_clear(sb);
        char *uri_credential = rd_kafka_aws_uri_encode(credential);
        
        char *amz_date = rd_kafka_aws_construct_amz_date(ymd, hms);
        char *uri_amz_date = rd_kafka_aws_uri_encode(amz_date);
        
        str_builder_add_str(sb, "Action=");
        str_builder_add_str(sb, uri_action);
        str_builder_add_str(sb, "&");
        str_builder_add_str(sb, "X-Amz-Algorithm=AWS4-HMAC-SHA256&");
        str_builder_add_str(sb, "X-Amz-Credential=");
        str_builder_add_str(sb, uri_credential);
        str_builder_add_str(sb, "&");
        str_builder_add_str(sb, "X-Amz-Date=");
        str_builder_add_str(sb, uri_amz_date);
        str_builder_add_str(sb, "&");
        str_builder_add_str(sb, "X-Amz-Expires=900&");  // AWS recommends 900 seconds
        
        if (aws_security_token != NULL) {
            char *uri_amz_security_token = rd_kafka_aws_uri_encode(aws_security_token);
            
            str_builder_add_str(sb, "X-Amz-Security-Token=");
            str_builder_add_str(sb, uri_amz_security_token);
            str_builder_add_str(sb, "&");
            
            RD_IF_FREE(uri_amz_security_token, rd_free);
        }
        
        str_builder_add_str(sb, "X-Amz-SignedHeaders=host");
        
        char *canonical_query_string = str_builder_dump(sb);
        
        str_builder_destroy(sb);
        
        RD_IF_FREE(uri_action, rd_free);
        RD_IF_FREE(credential_scope, rd_free);
        RD_IF_FREE(credential, rd_free);
        RD_IF_FREE(uri_credential, rd_free);
        RD_IF_FREE(amz_date, rd_free);
        RD_IF_FREE(uri_amz_date, rd_free);
        
        rd_kafka_dbg(rk, SECURITY, "AWS", "canonical_query_string=\"%s\"", 
            canonical_query_string);

        return canonical_query_string;
}

/**
 * @brief Generates a generic canonical request
 * @remark canonical_request will be allocated and must be freed.
 */
static char *rd_kafka_aws_build_canonical_request (const char *method,
                                        const char *canonical_query_string,
                                        const char *canonical_headers,
                                        const char *signed_headers,
                                        const char *request_parameters,
                                        const EVP_MD *md) {
        unsigned char md_value[EVP_MAX_MD_SIZE];
        unsigned int md_len, i;
        EVP_MD_CTX *mdctx;
        
        mdctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(mdctx, md, NULL);
        EVP_DigestUpdate(mdctx, request_parameters, strlen(request_parameters));
        EVP_DigestFinal_ex(mdctx, md_value, &md_len);
        EVP_MD_CTX_free(mdctx);

        char payload_hash[65];
        for (i = 0; i < md_len; i++)
               sprintf(&(payload_hash[i * 2]), "%02x", md_value[i]);  // save string in hex base 16
        payload_hash[64] = '\0';
        
        str_builder_t *sb;
        sb = str_builder_create();
        str_builder_add_str(sb, method);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, "/");  // canonical URI (usually "/")
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, canonical_query_string);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, canonical_headers);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, signed_headers);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, payload_hash);
        
        char *canonical_request = str_builder_dump(sb);
        
        str_builder_destroy(sb);
        
        return canonical_request;
}

/**
 * @brief Generates a string_to_sign
 * @remark string_to_sign will be allocated and must be freed.
 */
static char *rd_kafka_aws_build_string_to_sign (const char *algorithm,
                                        const char *credential_scope,
                                        const char *amz_date,
                                        const char *canonical_request,
                                        const EVP_MD *md) {
        unsigned char md_value[EVP_MAX_MD_SIZE];
        unsigned int md_len, i;
        EVP_MD_CTX *mdctx;
        
        mdctx = EVP_MD_CTX_new();
        EVP_DigestInit_ex(mdctx, md, NULL);
        EVP_DigestUpdate(mdctx, canonical_request, strlen(canonical_request));
        EVP_DigestFinal_ex(mdctx, md_value, &md_len);
        EVP_MD_CTX_free(mdctx);

        char hashed_canonical_request[65];
        for (i = 0; i < md_len; i++)
               sprintf(&(hashed_canonical_request[i * 2]), "%02x", md_value[i]);  // save string in hex base 16
        hashed_canonical_request[64] = '\0';
        
        str_builder_t *sb;
        sb = str_builder_create();
        str_builder_add_str(sb, algorithm);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, amz_date);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, credential_scope);
        str_builder_add_str(sb, "\n");
        str_builder_add_str(sb, hashed_canonical_request);
        
        char *string_to_sign = str_builder_dump(sb);
        
        str_builder_destroy(sb);

        return string_to_sign;
}

/**
 * @brief Generates a signature
 * @remark signature will be allocated and must be freed.
 */
static char *rd_kafka_aws_build_signature (const char *aws_secret_access_key, 
                                    const char *aws_region,
                                    const char *ymd,
                                    const char *aws_service,
                                    const char *string_to_sign) {
        unsigned int i;

        str_builder_t *sb;
        sb = str_builder_create();
        str_builder_add_str(sb, "AWS4");
        str_builder_add_str(sb, aws_secret_access_key);
        char *date_key = str_builder_dump(sb);
        
        str_builder_destroy(sb);
        
        unsigned char hmac_date_key[EVP_MAX_MD_SIZE];
        unsigned int hmac_date_key_len = sizeof(hmac_date_key);
        rd_kafka_aws_hmac_sha256(
            (unsigned char *)date_key, strlen(date_key),
            (unsigned char *)ymd, strlen(ymd),
            hmac_date_key, &hmac_date_key_len);
        
        unsigned char hmac_date_region_key[EVP_MAX_MD_SIZE];
        unsigned int hmac_date_region_key_len = sizeof(hmac_date_region_key);
        rd_kafka_aws_hmac_sha256(
            hmac_date_key, hmac_date_key_len,
            (unsigned char *)aws_region, strlen(aws_region),
            hmac_date_region_key, &hmac_date_region_key_len);
      
        unsigned char hmac_date_region_service_key[EVP_MAX_MD_SIZE];
        unsigned int hmac_date_region_service_key_len
            = sizeof(hmac_date_region_service_key);
        rd_kafka_aws_hmac_sha256(
            hmac_date_region_key, hmac_date_region_key_len,
            (unsigned char *)aws_service, strlen(aws_service),
            hmac_date_region_service_key,
            &hmac_date_region_service_key_len);
              
        unsigned char hmac_signing_key[EVP_MAX_MD_SIZE];
        unsigned int hmac_signing_key_len = sizeof(hmac_signing_key);
        rd_kafka_aws_hmac_sha256(
            hmac_date_region_service_key, hmac_date_region_service_key_len,
            (unsigned char *)"aws4_request", strlen("aws4_request"),
            hmac_signing_key, &hmac_signing_key_len);

        unsigned char hmac_signature[EVP_MAX_MD_SIZE];
        unsigned int hmac_signature_len = sizeof(hmac_signature);
        rd_kafka_aws_hmac_sha256(
            hmac_signing_key, hmac_signing_key_len,
            (unsigned char *)string_to_sign, strlen(string_to_sign),
            hmac_signature, &hmac_signature_len);
        
        char res_hexstring[EVP_MAX_MD_SIZE * 2 + 1];
        for (i = 0; i < hmac_signature_len; i++)
               sprintf(&(res_hexstring[i * 2]), "%02x", hmac_signature[i]);  // save string in hex base 16
        res_hexstring[2 * hmac_signature_len] = '\0';
        
        char *signature = rd_strdup(res_hexstring);
        
        RD_IF_FREE(date_key, rd_free);
        
        return signature;
}

int rd_kafka_aws_send_request (rd_kafka_t* rk,
                        rd_kafka_aws_credential_t *credential,
                        const char *ymd,
                        const char *hms,
                        const char *host,
                        const char *aws_access_key_id,
                        const char *aws_secret_access_key,
                        const char *aws_region,
                        const char *aws_service,
                        const char *method,
                        const char *algorithm,
                        const char *canonical_headers,
                        const char *signed_headers,
                        const char *request_parameters,
                        const EVP_MD *md,
                        char* errbuf,
                        size_t errbuf_size) {
                        
        int ret = RD_KAFKA_RESP_ERR_UNKNOWN;
        
        char *canonical_request = rd_kafka_aws_build_canonical_request(
                method,
                "",
                canonical_headers,
                signed_headers,
                request_parameters,
                md
        );

        rd_kafka_dbg(rk, SECURITY, "AWS", "canonical_request=\"%s\"", 
            canonical_request);

        char *credential_scope = rd_kafka_aws_construct_credential_scope(
                ymd,
                aws_region,
                aws_service
        );
        char *amz_date = rd_kafka_aws_construct_amz_date(ymd, hms);
        char *string_to_sign = rd_kafka_aws_build_string_to_sign(
                algorithm,
                credential_scope,
                amz_date,
                canonical_request,
                md
        );

        rd_kafka_dbg(rk, SECURITY, "AWS", "string_to_sign=\"%s\"", 
            string_to_sign);

        char *signature = rd_kafka_aws_build_signature(
                aws_secret_access_key,
                aws_region,
                ymd,
                aws_service,
                string_to_sign
        );
        char *authorization_header = rd_kafka_aws_construct_authorization_header(
                algorithm,
                aws_access_key_id,
                credential_scope,
                signed_headers,
                signature
        );

        CURL *curl;
        CURLcode res;
        curl = curl_easy_init();

        curl_in_mem_buf req = {.buffer = NULL, .len = 0, .buflen = 0};

        if (curl) {
                char curl_errbuf[CURL_ERROR_SIZE] = "";
                char *curl_host = NULL;
                char *curl_host_header = NULL;
                char *curl_content_length_header = NULL;
                char *curl_auth_header = NULL;
                char *curl_amz_date_header = NULL;
                char *curl_amz_security_token_header = NULL;
                char content_length[256];
                struct curl_slist *headers = NULL;
                
                str_builder_t *sb;
                sb = str_builder_create();

                curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_errbuf);
                curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION,
                    rd_kafak_aws_curl_debug_callback);
                curl_easy_setopt(curl, CURLOPT_DEBUGDATA, rk);
                curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

                if (rk->rk_conf.ssl.cert_location) {
                        curl_easy_setopt(curl, CURLOPT_SSLCERT,
                                         rk->rk_conf.ssl.cert_location);
                }
#               ifdef CURLOPTTYPE_BLOB
                if (rk->rk_conf.ssl.cert_pem) {
                        struct curl_blob blob;
                        blob.data = rk->rk_conf.ssl.cert_pem;
                        blob.len = strlen(rk->rk_conf.ssl.cert_pem);
                        blob.flags = CURL_BLOB_COPY;
                        curl_easy_setopt(curl, CURLOPT_SSLCERT_BLOB, &blob);
                        curl_easy_setopt(curl, CURLOPT_SSLCERTTYPE, "PEM");
                }
#               endif
                if (rk->rk_conf.ssl.key_location) {
                        curl_easy_setopt(curl, CURLOPT_SSLKEY,
                                         rk->rk_conf.ssl.key_location);
                }
#               ifdef CURLOPTTYPE_BLOB
                if (rk->rk_conf.ssl.key_pem) {
                        struct curl_blob blob;
                        blob.data = rk->rk_conf.ssl.key_pem;
                        blob.len = strlen(rk->rk_conf.ssl.key_pem);
                        blob.flags = CURL_BLOB_COPY;
                        curl_easy_setopt(curl, CURLOPT_SSLKEY_BLOB, &blob);
                        curl_easy_setopt(curl, CURLOPT_SSLKEYTYPE, "PEM");
                }
#               endif
                if (rk->rk_conf.ssl.key_password) {
                        curl_easy_setopt(curl, CURLOPT_KEYPASSWD,
                                         rk->rk_conf.ssl.key_password);
                }
                if (rk->rk_conf.ssl.ca_location) {
                        curl_easy_setopt(curl, CURLOPT_CAPATH,
                                         rk->rk_conf.ssl.ca_location);
                }
#               ifdef CURLOPTTYPE_BLOB
                if (rk->rk_conf.ssl.ca_pem) {
                        struct curl_blob blob;
                        blob.data = rk->rk_conf.ssl.ca_pem;
                        blob.len = strlen(rk->rk_conf.ssl.ca_pem);
                        blob.flags = CURL_BLOB_COPY;
                        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
                }
#               endif
        
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);

                str_builder_add_str(sb, "https://");
                str_builder_add_str(sb, host);
                curl_host = str_builder_dump(sb);
                str_builder_clear(sb);
                curl_easy_setopt(curl, CURLOPT_URL, curl_host);

                curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
                curl_easy_setopt(curl, CURLOPT_DEFAULT_PROTOCOL, "https");

                req.buffer = rd_malloc(CHUNK_SIZE);
                req.buflen = CHUNK_SIZE;

                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rd_kafka_aws_curl_write_callback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&req);

                /* Set Curl data */
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_parameters);

                // /* Set Curl headers */
                str_builder_add_str(sb, "Host: ");
                str_builder_add_str(sb, host);
                curl_host_header = str_builder_dump(sb);
                str_builder_clear(sb);
                headers = curl_slist_append(headers, curl_host_header);
                headers = curl_slist_append(headers, "User-Agent: librdkafka");

                rd_snprintf(content_length, sizeof(content_length), "%zu", strlen(request_parameters));
                str_builder_add_str(sb, "Content-Length: ");
                str_builder_add_str(sb, content_length);
                curl_content_length_header = str_builder_dump(sb);
                str_builder_clear(sb);
                headers = curl_slist_append(headers, curl_content_length_header);
                headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded; charset=utf-8");

                str_builder_add_str(sb, "Authorization: ");
                str_builder_add_str(sb, authorization_header);
                curl_auth_header = str_builder_dump(sb);
                str_builder_clear(sb);
                headers = curl_slist_append(headers, curl_auth_header);

                str_builder_add_str(sb, "X-Amz-Date: ");
                str_builder_add_str(sb, amz_date);
                curl_amz_date_header = str_builder_dump(sb);
                str_builder_clear(sb);
                headers = curl_slist_append(headers, curl_amz_date_header);
                headers = curl_slist_append(headers, "Accept-Encoding: gzip");

                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    if (errbuf) rd_strlcpy(errbuf, curl_errbuf, errbuf_size);
                    goto cleanup_and_exit;
                }

                rd_kafka_dbg(rk, SECURITY, "AWS", "curl_easy_perform() -> %d", (int) res);
                
                /* libxml2 calls are not multi-thread safe. Guard these with
                 * a global (not just per-rk) lock. */
                {
                        static rd_atomic32_t has_been_called;
                        static mtx_t libxml2_lock;
                        if (!rd_atomic32_exchange(&has_been_called, 1)) {
                                mtx_init(&libxml2_lock, mtx_plain);
                        }
                        mtx_lock(&libxml2_lock);
        
                        xmlDoc* document = xmlReadMemory((char*) req.buffer,
                            req.len, "assume_role_response.xml", NULL, 0);
                        xmlNode* cur = NULL;
                        if (document == NULL) goto done_libxml2;

                        cur = xmlDocGetRootElement(document);
                        if (xmlStrcmp(cur->name,
                            (const xmlChar *)"ErrorResponse") == 0) {
                                if (!errbuf) goto done_libxml2;
                                cur = cur->children;
                                while (cur != NULL) {
                                        if (!xmlStrcmp(cur->name,
                                            (const xmlChar *)"Error")) {
                                                break;
                                        }
                                        cur = cur->next;
                                }
                                if (cur == NULL) goto done_libxml2;

                                cur = cur->children;
                                while (cur != NULL) {
                                        if (!xmlStrcmp(cur->name,
                                            (const xmlChar *)"Message")) {
                                                xmlChar* content
                                                    = xmlNodeListGetString(
                                                    document, cur->children, 1);
                                                rd_strlcpy(errbuf,
                                                    (const char*) content,
                                                    errbuf_size);
                                                xmlFree(content);
                                        }
                                        cur = cur->next;
                                }
                                goto done_libxml2;
                        }

                        cur = cur->children;
                        while (cur != NULL) {
                                if (!xmlStrcmp(cur->name, (const xmlChar *)"AssumeRoleResult")) {
                                        break;
                                }
                                cur = cur->next;
                        }
        
                        if (cur == NULL) goto done_libxml2;
                        cur = cur->children;
                        while (cur != NULL) {
                                if (!xmlStrcmp(cur->name, (const xmlChar *)"Credentials")) {
                                        break;
                                }
                                cur = cur->next;
                        }

                        if (cur == NULL) goto done_libxml2;
                        cur = cur->children;
                        while (cur != NULL) {
                                if (!xmlStrcmp(cur->name, 
                                    (const xmlChar*) "AccessKeyId")) {
                                        xmlChar* content = xmlNodeListGetString(
                                            document, cur->children, 1);
                                        credential->aws_access_key_id
                                            = rd_strdup((const char *)content);
                                        xmlFree(content);
                                }
        
                                if (!xmlStrcmp(cur->name,
                                    (const xmlChar*) "SecretAccessKey")) {
                                        xmlChar* content = xmlNodeListGetString(
                                            document, cur->children, 1);
                                        credential->aws_secret_access_key
                                            = rd_strdup((const char *)content);
                                        xmlFree(content);
                                }
        
                                if (!xmlStrcmp(cur->name,
                                    (const xmlChar*) "SessionToken")) {
                                        xmlChar* content = xmlNodeListGetString(
                                            document, cur->children, 1);
                                        credential->aws_security_token
                                            = rd_strdup((const char *)content);
                                        xmlFree(content);
                                }
        
                                if (!xmlStrcmp(cur->name,
                                    (const xmlChar*) "Expiration")) {
                                        /* Expiration timestamps are in
                                         * YYYY-MM-DDTHH:MM:SS.SSSZ format */
                                        xmlChar* content = xmlNodeListGetString(
                                            document, cur->children, 1);
                                        struct tm tm = RD_ZERO_INIT;
                                        float secf = 0.;
                                        if (sscanf((const char*) content, 
                                            "%4d-%2d-%2dT%2d:%2d:%fZ",
                                            &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                                            &tm.tm_hour, &tm.tm_min, &secf) == 6) {
                                                tm.tm_year -= 1900;
                                                tm.tm_mon -= 1;
                                                tm.tm_sec = secf;
                                                time_t epochSec = mktime(&tm);
                                                if (epochSec > 0) {
                                                        credential->md_lifetime_ms
                                                            = epochSec * 1000l;
                                                }
                                        }
                                        xmlFree(content);
                                }
        
                                cur = cur->next;
                        }
                        
                        ret = RD_KAFKA_RESP_ERR_NO_ERROR;
                        
                done_libxml2:
                        if (document) xmlFreeDoc(document);
                        mtx_unlock(&libxml2_lock);
                }

        cleanup_and_exit:

                RD_IF_FREE(req.buffer, rd_free);
                RD_IF_FREE(curl_host, rd_free);
                RD_IF_FREE(curl_host_header, rd_free);
                RD_IF_FREE(curl_content_length_header, rd_free);
                RD_IF_FREE(curl_auth_header, rd_free);
                RD_IF_FREE(curl_amz_date_header, rd_free);
                RD_IF_FREE(curl_amz_security_token_header, rd_free);
                
                if (sb) str_builder_destroy(sb);
        } /* if (curl) */
        curl_easy_cleanup(curl);
        
        /* backstop error string */
        if ((ret != RD_KAFKA_RESP_ERR_NO_ERROR) && errbuf && !errbuf[0]) {
            rd_strlcpy(errbuf, "AWS send request: unknown error", errbuf_size);
        }

        return ret;
}

/**
 * @brief Generates a sasl_payload
 * @remark sasl_payload will be allocated and must be freed.
 */
char *rd_kafka_aws_build_sasl_payload (rd_kafka_t* rk,
                                const char *ymd,
                                const char *hms,
                                const char *host,
                                const char *aws_access_key_id, 
                                const char *aws_secret_access_key,
                                const char *aws_security_token,
                                const char *aws_region,
                                const char *aws_service,
                                const char *method,
                                const char *algorithm,
                                const char *canonical_headers,
                                const char *canonical_querystring,
                                const char *signed_headers,
                                const char *request_parameters,
                                const EVP_MD *md) {
        char *canonical_request = rd_kafka_aws_build_canonical_request(
                method,
                canonical_querystring,
                canonical_headers,
                signed_headers,
                request_parameters,
                md
        );

        rd_kafka_dbg(rk, SECURITY, "AWSSASLPAYLOAD", "canonical_request=\"%s\"", 
            canonical_request);

        char *credential_scope = rd_kafka_aws_construct_credential_scope(
                ymd,
                aws_region,
                aws_service
        );

        rd_kafka_dbg(rk, SECURITY, "AWSSASLPAYLOAD", "credential_scope=\"%s\"", 
            credential_scope);

        char *amz_date = rd_kafka_aws_construct_amz_date(ymd, hms);
        char *string_to_sign = rd_kafka_aws_build_string_to_sign(
                algorithm,
                credential_scope,
                amz_date,
                canonical_request,
                md
        );

        rd_kafka_dbg(rk, SECURITY, "AWSSASLPAYLOAD", "string_to_sign=\"%s\"", 
            string_to_sign);

        char *signature = rd_kafka_aws_build_signature(
                aws_secret_access_key,
                aws_region,
                ymd,
                aws_service,
                string_to_sign
        );

        rd_kafka_dbg(rk, SECURITY, "AWSSASLPAYLOAD", "signature=\"%s\"", 
            signature);

        /* Construct JSON payload */
        str_builder_t *sb;
        sb = str_builder_create();
        str_builder_add_str(sb, "{\"version\":\"2020_10_22\",");
        str_builder_add_str(sb, "\"host\":\"");
        str_builder_add_str(sb, host);
        str_builder_add_str(sb, "\",");
        str_builder_add_str(sb, "\"user-agent\":\"librdkafka\",");
        str_builder_add_str(sb, "\"action\":\"");
        str_builder_add_str(sb, "kafka-cluster:Connect");
        str_builder_add_str(sb, "\",");
        str_builder_add_str(sb, "\"x-amz-algorithm\":\"AWS4-HMAC-SHA256\",");
        str_builder_add_str(sb, "\"x-amz-credential\":\"");
        str_builder_add_str(sb, aws_access_key_id);
        str_builder_add_str(sb, "/");
        str_builder_add_str(sb, credential_scope);
        str_builder_add_str(sb, "\",");
        str_builder_add_str(sb, "\"x-amz-date\":\"");
        str_builder_add_str(sb, amz_date);
        str_builder_add_str(sb, "\",");
        
        if (aws_security_token != NULL) {
            str_builder_add_str(sb, "\"x-amz-security-token\":\"");
            str_builder_add_str(sb, aws_security_token);
            str_builder_add_str(sb, "\",");
        }
        
        str_builder_add_str(sb, "\"x-amz-signedheaders\":\"host\",");
        str_builder_add_str(sb, "\"x-amz-expires\":\"900\",");
        str_builder_add_str(sb, "\"x-amz-signature\":\"");
        str_builder_add_str(sb, signature);
        str_builder_add_str(sb, "\"}");

        char *sasl_payload = str_builder_dump(sb);
        str_builder_destroy(sb);

        RD_IF_FREE(canonical_request, rd_free);
        RD_IF_FREE(amz_date, rd_free);
        RD_IF_FREE(credential_scope, rd_free);
        RD_IF_FREE(string_to_sign, rd_free);
        RD_IF_FREE(signature, rd_free);
        
        rd_kafka_dbg(rk, SECURITY, "AWSSASLPAYLOAD", "sasl_payload=\"%s\"", 
            sasl_payload);

        return sasl_payload;
}


/**
 * @name Unit tests
 */

/**
 * @brief Verify that a sasl payload JSON can be formed properly.
 */
static int unittest_build_sasl_payload (void) {
        RD_UT_BEGIN();

        const EVP_MD *md = EVP_get_digestbyname("SHA256");
        char *ymd = "20100101";
        char *hms = "000000";
        char *aws_region = "us-east-1";
        char *aws_service = "kafka-cluster";
        char *aws_access_key_id = "AWS_ACCESS_KEY_ID";
        char *aws_secret_access_key = "AWS_SECRET_ACCESS_KEY";
        char *aws_security_token = NULL;
        char *algorithm = "AWS4-HMAC-SHA256";
        char *canonical_headers = "host:hostname";
        char *signed_headers = "host";
        char *host = "hostname";
        char *method = "GET";
        char *request_parameters = "";

        char *canonical_querystring = rd_kafka_aws_build_sasl_canonical_querystring(
                NULL,
                "kafka-cluster:Connect",
                aws_access_key_id,
                aws_region,
                ymd,
                hms,
                aws_service,
                aws_security_token
        );

        char *expected_canonical_querystring = "Action=kafka-cluster%3AConnect&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AWS_ACCESS_KEY_ID%2F20100101%2Fus-east-1%2Fkafka-cluster%2Faws4_request&X-Amz-Date=20100101T000000Z&X-Amz-Expires=900&X-Amz-SignedHeaders=host";
        RD_UT_ASSERT(strcmp(expected_canonical_querystring, canonical_querystring) == 0, "expected: %s\nactual: %s", expected_canonical_querystring, canonical_querystring);
        
        char *sasl_payload = rd_kafka_aws_build_sasl_payload(NULL,
                ymd,
                hms,
                host,
                aws_access_key_id,
                aws_secret_access_key,
                aws_security_token,
                aws_region,
                aws_service,
                method,
                algorithm,
                canonical_headers,
                canonical_querystring,
                signed_headers,
                request_parameters,
                md
        );
        
        const char *expected = 
            "{\"version\":\"2020_10_22\",\"host\":\"hostname\","
            "\"user-agent\":\"librdkafka\",\"action\":\"kafka-cluster:Connect\","
            "\"x-amz-algorithm\":\"AWS4-HMAC-SHA256\","
            "\"x-amz-credential\":\"AWS_ACCESS_KEY_ID/20100101/us-east-1/kafka-cluster/aws4_request\","
            "\"x-amz-date\":\"20100101T000000Z\","
            "\"x-amz-signedheaders\":\"host\","
            "\"x-amz-expires\":\"900\","
            "\"x-amz-signature\":\"d3eeeddfb2c2b76162d583d7499c2364eb9a92b248218e31866659b18997ef44\"}";
        RD_UT_ASSERT(strcmp(expected, sasl_payload) == 0, "expected: %s\nactual: %s", expected, sasl_payload);
        
        RD_IF_FREE(sasl_payload, rd_free);
        RD_UT_PASS();
}

/**
 * @brief Verify that a request JSON can be formed properly.
 */
static int unittest_build_sts_request (void) {
        RD_UT_BEGIN();

        const EVP_MD *md = EVP_get_digestbyname("SHA256");
        char *ymd = "20210910";
        char *hms = "190714";
        char *aws_region = "us-east-1";
        char *aws_service = "sts";
        char *aws_access_key_id = "TESTKEY";
        char *aws_secret_access_key = "TESTSECRET";
        char *algorithm = "AWS4-HMAC-SHA256";
        char *canonical_headers = "content-length:171\ncontent-type:application/x-www-form-urlencoded; charset=utf-8\nhost:sts.amazonaws.com\nx-amz-date:20210910T190714Z";
        char *signed_headers = "content-length;content-type;host;x-amz-date";
        char *method = "POST";
        char *canonical_querystring = "";
        char *request_parameters = "Action=AssumeRole&DurationSeconds=900&RoleArn=arn%3Aaws%3Aiam%3A%3A789750736714%3Arole%2FIdentity_Account_Access_Role&RoleSessionName=librdkafka_session&Version=2011-06-15";

        char *canonical_request = rd_kafka_aws_build_canonical_request(
                method,
                canonical_querystring,
                canonical_headers,
                signed_headers,
                request_parameters,
                md
        );
        // printf("DEBUGGING: canonical request\n%s\n", canonical_request);
        char *expected_canonical_request = "POST\n/\n\ncontent-length:171\ncontent-type:application/x-www-form-urlencoded; charset=utf-8\nhost:sts.amazonaws.com\nx-amz-date:20210910T190714Z\n\ncontent-length;content-type;host;x-amz-date\n452ff5eb28a9cd9928d29dd4e27815e5e4f0f3e8e7e59b37700f55584f43ffaa";
        RD_UT_ASSERT(strcmp(canonical_request, expected_canonical_request) == 0, "expected: %s\nactual: %s", expected_canonical_request, canonical_request);

        char *credential_scope = rd_kafka_aws_construct_credential_scope(
                ymd,
                aws_region,
                aws_service
        );
        char *expected_credential_scope = "20210910/us-east-1/sts/aws4_request";
        RD_UT_ASSERT(strcmp(credential_scope, expected_credential_scope) == 0, "expected: %s\nactual: %s", expected_credential_scope, credential_scope);

        char *amz_date = rd_kafka_aws_construct_amz_date(ymd, hms);
        char *string_to_sign = rd_kafka_aws_build_string_to_sign(
                algorithm,
                credential_scope,
                amz_date,
                canonical_request,
                md
        );
        // printf("DEBUGGING: string to sign\n%s\n", string_to_sign);
        char *expected_string_to_sign = "AWS4-HMAC-SHA256\n20210910T190714Z\n20210910/us-east-1/sts/aws4_request\nd66dff688ce93a268731fee21e3751669e2c27b8b54ce6d2d627b2c6f7049a7f";
        RD_UT_ASSERT(strcmp(string_to_sign, expected_string_to_sign) == 0, "expected: %s\nactual: %s", expected_string_to_sign, string_to_sign);
        
        char *signature = rd_kafka_aws_build_signature(
                aws_secret_access_key,
                aws_region,
                ymd,
                aws_service,
                string_to_sign
        );
        // printf("DEBUGGING: signature\n%s\n", signature);
        char *expected_signature = "a825a6136b83c3feb7993b9d2947f6e479901f805089b08f717c0f2a03cd98f0";
        RD_UT_ASSERT(strcmp(signature, expected_signature) == 0, "expected: %s\nactual: %s", expected_signature, signature);

        char *authorization_header = rd_kafka_aws_construct_authorization_header(
                algorithm,
                aws_access_key_id,
                credential_scope,
                signed_headers,
                signature
        );
        char *expected_authorization_header = "AWS4-HMAC-SHA256 Credential=TESTKEY/20210910/us-east-1/sts/aws4_request, SignedHeaders=content-length;content-type;host;x-amz-date, Signature=a825a6136b83c3feb7993b9d2947f6e479901f805089b08f717c0f2a03cd98f0";
        RD_UT_ASSERT(strcmp(authorization_header, expected_authorization_header) == 0, "expected: %s\nactual: %s", expected_authorization_header, authorization_header);

        RD_IF_FREE(canonical_request, rd_free);
        RD_IF_FREE(string_to_sign, rd_free);
        RD_IF_FREE(signature, rd_free);
        RD_IF_FREE(authorization_header, rd_free);
        RD_IF_FREE(credential_scope, rd_free);
        RD_IF_FREE(amz_date, rd_free);
        RD_UT_PASS();
}

/**
 * @brief Verify that a signature can be calculated properly.
 */
static int unittest_build_signature (void) {
        RD_UT_BEGIN();

        const EVP_MD *md = EVP_get_digestbyname("SHA256");
        char *ymd = "20100101";
        char *hms = "000000";
        char *aws_region = "us-east-1";
        char *aws_service = "kafka-cluster";
        char *aws_secret_access_key = "AWS_SECRET_ACCESS_KEY";
        char *algorithm = "AWS4-HMAC-SHA256";
        char *canonical_headers = "host:hostname";
        char *signed_headers = "host";
        char *method = "GET";
        char *canonical_querystring = "Action=kafka-cluster%3AConnect&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AWS_ACCESS_KEY_ID%2F20100101%2Fus-east-1%2Fkafka-cluster%2Faws4_request&X-Amz-Date=20100101T000000Z&X-Amz-Expires=900&X-Amz-SignedHeaders=host";
        char *request_parameters = "";

        char *canonical_request = rd_kafka_aws_build_canonical_request(
                method,
                canonical_querystring,
                canonical_headers,
                signed_headers,
                request_parameters,
                md
        );

        char *credential_scope = rd_kafka_aws_construct_credential_scope(
                ymd,
                aws_region,
                aws_service
        );
        char *expected_credential_scope = "20100101/us-east-1/kafka-cluster/aws4_request";
        RD_UT_ASSERT(strcmp(credential_scope, expected_credential_scope) == 0, "expected: %s\nactual: %s", expected_credential_scope, credential_scope);

        char *amz_date = rd_kafka_aws_construct_amz_date(ymd, hms);
        char *string_to_sign = rd_kafka_aws_build_string_to_sign(
                algorithm,
                credential_scope,
                amz_date,
                canonical_request,
                md
        );
        // printf("DEBUGGING: string to sign\n%s\n", string_to_sign);
        char *expected_string_to_sign = "AWS4-HMAC-SHA256\n20100101T000000Z\n20100101/us-east-1/kafka-cluster/aws4_request\n8a719fb6d4b33f7d9c5b25b65af85a44d3627bdca66e1287b1a366fa90bafaa1";
        RD_UT_ASSERT(strcmp(string_to_sign, expected_string_to_sign) == 0, "expected: %s\nactual: %s", expected_string_to_sign, string_to_sign);

        char *signature = rd_kafka_aws_build_signature(
                aws_secret_access_key,
                aws_region,
                ymd,
                aws_service,
                string_to_sign
        );
        
        const char *expected = "d3eeeddfb2c2b76162d583d7499c2364eb9a92b248218e31866659b18997ef44";
        RD_UT_ASSERT(strcmp(expected, signature) == 0, "expected: %s\nactual: %s", expected, signature);
    
        RD_IF_FREE(canonical_request, rd_free);
        RD_IF_FREE(string_to_sign, rd_free);
        RD_IF_FREE(signature, rd_free);
        RD_IF_FREE(credential_scope, rd_free);
        RD_IF_FREE(amz_date, rd_free);
        RD_UT_PASS();
}

/**
 * @brief Verify that a canonical request can be formed properly.
 */
static int unittest_build_canonical_request_with_security_token (void) {       
        RD_UT_BEGIN();
        
        const EVP_MD *md = EVP_get_digestbyname("SHA256");
        char *canonical_headers = "host:hostname";
        char *signed_headers = "host";
        char *method = "GET";
        char *canonical_querystring = "Action=kafka-cluster%3AConnect&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AWS_ACCESS_KEY_ID%2F20100101%2Fus-east-1%2Fkafka-cluster%2Faws4_request&X-Amz-Date=20100101T000000Z&X-Amz-Expires=900&X-Amz-Security-Token=security-token&X-Amz-SignedHeaders=host";
        char *request_parameters = "";

        char *canonical_request = rd_kafka_aws_build_canonical_request(
                method,
                canonical_querystring,
                canonical_headers,
                signed_headers,
                request_parameters,
                md
        );
        
        const char *expected = 
            "GET\n/\n"
            "Action=kafka-cluster%3AConnect&"
            "X-Amz-Algorithm=AWS4-HMAC-SHA256&"
            "X-Amz-Credential=AWS_ACCESS_KEY_ID%2F20100101%2Fus-east-1%2Fkafka-cluster%2Faws4_request&"
            "X-Amz-Date=20100101T000000Z&"
            "X-Amz-Expires=900&"
            "X-Amz-Security-Token=security-token&"
            "X-Amz-SignedHeaders=host\n"
            "host:hostname\n\n"
            "host\n"
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        RD_UT_ASSERT(strcmp(expected, canonical_request) == 0, "expected: %s\nactual: %s", expected, canonical_request);
   
        RD_IF_FREE(canonical_request, rd_free);
        RD_UT_PASS();
}

/**
 * @brief Verify that a canonical request can be formed properly.
 */
static int unittest_build_canonical_request (void) {       
        RD_UT_BEGIN();

        const EVP_MD *md = EVP_get_digestbyname("SHA256");
        char *canonical_headers = "host:hostname";
        char *signed_headers = "host";
        char *method = "GET";
        char *canonical_querystring = "Action=kafka-cluster%3AConnect&X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AWS_ACCESS_KEY_ID%2F20100101%2Fus-east-1%2Fkafka-cluster%2Faws4_request&X-Amz-Date=20100101T000000Z&X-Amz-Expires=900&X-Amz-SignedHeaders=host";
        char *request_parameters = "";

        char *canonical_request = rd_kafka_aws_build_canonical_request(
                method,
                canonical_querystring,
                canonical_headers,
                signed_headers,
                request_parameters,
                md
        );
        
        const char *expected = 
            "GET\n/\n"
            "Action=kafka-cluster%3AConnect&"
            "X-Amz-Algorithm=AWS4-HMAC-SHA256&"
            "X-Amz-Credential=AWS_ACCESS_KEY_ID%2F20100101%2Fus-east-1%2Fkafka-cluster%2Faws4_request&"
            "X-Amz-Date=20100101T000000Z&"
            "X-Amz-Expires=900&"
            "X-Amz-SignedHeaders=host\n"
            "host:hostname\n\n"
            "host\n"
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
        RD_UT_ASSERT(strcmp(expected, canonical_request) == 0, "expected: %s\nactual: %s", expected, canonical_request);
   
        RD_IF_FREE(canonical_request, rd_free);
        RD_UT_PASS();
}

/**
 * @brief Verify that a uri encoding / escaping works as expected.
 */
static int unittest_uri_encode (void) {
        RD_UT_BEGIN();
        int test_str_size = strlen("testString-123/*&") + 1;
        char *test_str;
        
        test_str = rd_malloc(test_str_size);
        rd_snprintf(test_str, test_str_size, "testString-123/*&");

        char *retval = rd_kafka_aws_uri_encode(test_str);

        const char *expected = "testString-123%2F%2A%26";
        RD_UT_ASSERT(strcmp(expected, retval) == 0, "expected: %s\nactual: %s", expected, retval);

        RD_IF_FREE(retval, rd_free);
        RD_IF_FREE(test_str, rd_free);
        RD_UT_PASS();
}

int unittest_aws (void) {
        int fails = 0;

        fails += unittest_uri_encode();
        fails += unittest_build_canonical_request();
        fails += unittest_build_canonical_request_with_security_token();
        fails += unittest_build_signature();
        fails += unittest_build_sasl_payload();
        fails += unittest_build_sts_request();

        return fails;
}
