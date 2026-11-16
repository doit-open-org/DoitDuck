/*
 * BluFi's security as the EspBlufi app negotiates it: Diffie-Hellman with the
 * phone's p and g, the AES-128 key the MD5 of the shared secret, CFB-128 with the
 * frame's sequence number as the first byte of the IV, and CRC-16 checksums.
 *
 * From hi_aiot_solution's BluFi sample (Apache-2.0), on Mbed TLS instead of OpenSSL.
 */
#include "blufi_security.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/dhm.h"
#include "mbedtls/md5.h"
#include "mbedtls/platform_util.h"

#define BLUFI_DH_PARAM_MAX_LEN 1024U
#define BLUFI_DH_KEY_MAX_LEN 128U

#define BLUFI_NEG_SET_PARAM_LEN 0U
#define BLUFI_NEG_SET_PARAM_DATA 1U

struct blufi_security {
    mbedtls_aes_context aes;
    size_t expected_param_len;
    bool ready;
};

static int urandom(void *context, unsigned char *out, size_t len)
{
    (void)context;
    FILE *f = fopen("/dev/urandom", "rb");
    const int ok = f != NULL && fread(out, 1U, len, f) == len;
    if (f != NULL) {
        fclose(f);
    }
    return ok ? 0 : -1;
}

/* A field of the negotiation data: two bytes of length (big-endian), then the value. */
static int blufi_read_field(const uint8_t *data, size_t len, size_t *offset,
    const uint8_t **field, size_t *field_len)
{
    size_t current = *offset;
    size_t value_len;

    if (current > len || len - current < 2U) {
        return -1;
    }
    value_len = ((size_t)data[current] << 8U) | data[current + 1U];
    current += 2U;
    if (value_len == 0U || value_len > len - current) {
        return -1;
    }
    *field = data + current;
    *field_len = value_len;
    *offset = current + value_len;
    return 0;
}

/* p, g and the phone's public key in; our public key out, and the AES key from the shared secret. */
static int blufi_security_generate_key(blufi_security *security, const uint8_t *data,
    size_t len, uint8_t *response, size_t response_size, size_t *response_len)
{
    const uint8_t *p, *g, *peer;
    size_t p_len, g_len, peer_len, key_len, offset = 0U, shared_len = 0U;
    uint8_t shared[BLUFI_DH_KEY_MAX_LEN], key[16];
    mbedtls_dhm_context dhm;
    mbedtls_mpi P, G;
    int ret = -1;

    mbedtls_dhm_init(&dhm);
    mbedtls_mpi_init(&P);
    mbedtls_mpi_init(&G);
    if (blufi_read_field(data, len, &offset, &p, &p_len) != 0 ||
        blufi_read_field(data, len, &offset, &g, &g_len) != 0 ||
        blufi_read_field(data, len, &offset, &peer, &peer_len) != 0 ||
        offset != len || p_len > BLUFI_DH_KEY_MAX_LEN ||
        mbedtls_mpi_read_binary(&P, p, p_len) != 0 || mbedtls_mpi_read_binary(&G, g, g_len) != 0 ||
        mbedtls_dhm_set_group(&dhm, &P, &G) != 0) {
        goto cleanup;
    }
    /* Our public key as long as p is (leading zeros of p not counted), as OpenSSL's DH_size. */
    key_len = mbedtls_dhm_get_len(&dhm);
    if (key_len == 0U || key_len > response_size ||
        mbedtls_dhm_make_public(&dhm, (int)key_len, response, key_len, urandom, NULL) != 0 ||
        mbedtls_dhm_read_public(&dhm, peer, peer_len) != 0 ||
        mbedtls_dhm_calc_secret(&dhm, shared, sizeof(shared), &shared_len, urandom, NULL) != 0 ||
        mbedtls_md5(shared, shared_len, key) != 0 ||
        mbedtls_aes_setkey_enc(&security->aes, key, 128U) != 0) {
        goto cleanup;
    }
    *response_len = key_len;
    security->ready = true;
    ret = 0;

cleanup:
    mbedtls_platform_zeroize(shared, sizeof(shared));
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_mpi_free(&P);
    mbedtls_mpi_free(&G);
    mbedtls_dhm_free(&dhm);
    if (ret != 0) {
        blufi_security_reset(security);
    }
    return ret;
}

blufi_security *blufi_security_create(void)
{
    blufi_security *security = calloc(1U, sizeof(blufi_security));

    if (security != NULL) {
        mbedtls_aes_init(&security->aes);
    }
    return security;
}

void blufi_security_reset(blufi_security *security)
{
    if (security == NULL) {
        return;
    }
    mbedtls_aes_free(&security->aes);
    mbedtls_aes_init(&security->aes);
    security->expected_param_len = 0U;
    security->ready = false;
}

void blufi_security_destroy(blufi_security *security)
{
    if (security == NULL) {
        return;
    }
    mbedtls_aes_free(&security->aes);
    mbedtls_platform_zeroize(security, sizeof(*security));
    free(security);
}

/* First the length of the parameters, then the parameters: p, g and the phone's public key. */
int blufi_security_negotiate(blufi_security *security, const uint8_t *data, size_t len,
    uint8_t *response, size_t response_size, size_t *response_len)
{
    size_t param_len;

    if (security == NULL || data == NULL || response == NULL || response_len == NULL || len < 1U) {
        return -1;
    }
    *response_len = 0U;

    if (data[0] == BLUFI_NEG_SET_PARAM_LEN) {
        if (len != 3U) {
            return -1;
        }
        param_len = ((size_t)data[1] << 8U) | data[2];
        if (param_len == 0U || param_len > BLUFI_DH_PARAM_MAX_LEN) {
            return -1;
        }
        blufi_security_reset(security);
        security->expected_param_len = param_len;
        return 0;
    }

    if (data[0] != BLUFI_NEG_SET_PARAM_DATA || security->expected_param_len == 0U ||
        len - 1U != security->expected_param_len) {
        return -1;
    }
    param_len = security->expected_param_len;
    security->expected_param_len = 0U;
    return blufi_security_generate_key(security, data + 1U, param_len,
        response, response_size, response_len);
}

static int blufi_security_crypt(blufi_security *security, uint8_t sequence, uint8_t *data,
    size_t len, int mode)
{
    uint8_t iv[16] = {sequence};
    size_t offset = 0U;

    if (security == NULL || !security->ready || (data == NULL && len != 0U)) {
        return -1;
    }
    return mbedtls_aes_crypt_cfb128(&security->aes, mode, len, &offset, iv, data, data) == 0 ? 0 : -1;
}

int blufi_security_encrypt(blufi_security *security, uint8_t sequence, uint8_t *data, size_t len)
{
    return blufi_security_crypt(security, sequence, data, len, MBEDTLS_AES_ENCRYPT);
}

int blufi_security_decrypt(blufi_security *security, uint8_t sequence, uint8_t *data, size_t len)
{
    return blufi_security_crypt(security, sequence, data, len, MBEDTLS_AES_DECRYPT);
}

/* CRC-16-CCITT, initial value 0xFFFF, inverted. */
uint16_t blufi_security_checksum(const uint8_t *data, size_t len)
{
    uint16_t crc = UINT16_MAX;

    for (size_t i = 0U; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8U;
        for (unsigned int bit = 0U; bit < 8U; bit++) {
            crc = (crc & 0x8000U) != 0U ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
        }
    }
    return (uint16_t)~crc;
}

int blufi_security_ready(const blufi_security *security)
{
    return security != NULL && security->ready ? 1 : 0;
}
