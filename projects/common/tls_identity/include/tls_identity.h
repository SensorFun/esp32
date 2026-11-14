#pragma once
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t tls_identity_init(void);
const char *tls_identity_cert(void);
const char *tls_identity_key(void);
#ifdef __cplusplus
}
#endif
