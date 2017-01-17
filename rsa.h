#include "openssl/rsa.h"
#include "openssl/pem.h"
#include "openssl/evp.h"
#include "openssl/err.h"
#include "openssl/ssl.h"
#include "openssl/aes.h"
#define RSA_KEY_LENGTH 1024

int rsa_priv_decrypt(char *privkey,char *cipher,char *out_plain);
int rsa_pub_encrypt(char *pubkey,char *in_plain,char *cipher);
int rsa_gen_keys(const int g_nbits,char *pub_key,char *priv_key);
int encdec_init();
int aes_gen_key(unsigned char *aes_key,int n_bits);
int aes_cbc_enc(AES_KEY *enc_key,unsigned char *plain,unsigned char *cipher);
int aes_cbc_dec(AES_KEY *dec_key,unsigned char *cipher,unsigned char *plain);
