#ifndef BLUFI_SECURITY_H
#define BLUFI_SECURITY_H

#include <stddef.h>
#include <stdint.h>

typedef struct blufi_security blufi_security;

blufi_security *blufi_security_create(void);
void blufi_security_destroy(blufi_security *security);
void blufi_security_reset(blufi_security *security);
int blufi_security_negotiate(blufi_security *security, const uint8_t *data, size_t len,
    uint8_t *response, size_t response_size, size_t *response_len);
int blufi_security_encrypt(blufi_security *security, uint8_t sequence, uint8_t *data, size_t len);
int blufi_security_decrypt(blufi_security *security, uint8_t sequence, uint8_t *data, size_t len);
uint16_t blufi_security_checksum(const uint8_t *data, size_t len);
int blufi_security_ready(const blufi_security *security);

#endif
