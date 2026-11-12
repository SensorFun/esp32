#include "tls_identity.h"
#include "esp_random.h"
#include "esp_log.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include <stdio.h>
#include <string.h>

static char certificate[2048], private_key[1024], setup_password[17];
esp_err_t tls_identity_init(void) {
    if (certificate[0]) return ESP_OK;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_x509_crt parsed;
    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_x509_crt_init(&parsed);
    psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t key_id = 0;
    int ret = psa_crypto_init();
    psa_set_key_type(&attrs, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attrs, 256);
    psa_set_key_algorithm(&attrs, MBEDTLS_PK_ALG_ECDSA(PSA_ALG_ANY_HASH));
    psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH | PSA_KEY_USAGE_EXPORT);
    if (!ret) ret = psa_generate_key(&attrs, &key_id);
    if (!ret) ret = mbedtls_pk_wrap_psa(&key, key_id);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    unsigned char serial[16];
    esp_fill_random(serial, sizeof(serial));
    serial[0] = (serial[0] & 0x7f) | 1;
    if (!ret) ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
    if (!ret) ret = mbedtls_x509write_crt_set_subject_name(&crt, "CN=Local Camera");
    if (!ret) ret = mbedtls_x509write_crt_set_issuer_name(&crt, "CN=Local Camera");
    if (!ret) ret = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20491231235959");
    if (!ret) ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
    if (!ret) ret = mbedtls_x509write_crt_pem(&crt, (unsigned char *)certificate, sizeof(certificate));
    if (!ret) ret = mbedtls_pk_write_key_pem(&key, (unsigned char *)private_key, sizeof(private_key));
    if (!ret) ret = mbedtls_x509_crt_parse(&parsed, (unsigned char *)certificate, strlen(certificate) + 1);
    if (!ret) {
        unsigned char digest[32], secret[8];
        char fingerprint[65];
        size_t digest_len;
        ret = psa_hash_compute(PSA_ALG_SHA_256, parsed.raw.p, parsed.raw.len, digest, sizeof(digest), &digest_len);
        if (ret || digest_len != sizeof(digest)) { ret = -1; goto cleanup; }
        esp_fill_random(secret, sizeof(secret));
        for (int i = 0; i < 32; ++i) {
            snprintf(fingerprint + i * 2, 3, "%02X", digest[i]);

        }
        for (size_t i = 0; i < sizeof(secret); ++i) snprintf(setup_password + i * 2, 3, "%02x", secret[i]);
        memset(secret, 0, sizeof(secret));
        ESP_LOGI("local_tls", "HTTPS certificate SHA256: %s", fingerprint);
    }
cleanup:
    if (ret) {
        memset(certificate, 0, sizeof(certificate));
        memset(private_key, 0, sizeof(private_key));
        ESP_LOGE("local_tls", "Identity generation failed: %d", ret);
    }
    mbedtls_x509_crt_free(&parsed);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    if (key_id) psa_destroy_key(key_id);
    psa_reset_key_attributes(&attrs);
    return ret ? ESP_FAIL : ESP_OK;
}
const char *tls_identity_cert(void) { return certificate; }
const char *tls_identity_key(void) { return private_key; }
const char *tls_identity_setup_password(void) { return setup_password; }
