/*
 * Support for ePass2003 smart cards 
 *
 * Copyright (C) 2008, Weitao Sun <weitao@ftsafe.com>
 * Copyright (C) 2011, Xiaoshuo Wu <xiaoshuo@ftsafe.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#ifdef ENABLE_OPENSSL		/* empty file without openssl */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#include "internal.h"
#include "asn1.h"
#include "cardctl.h"

static struct sc_atr_table epass2003_atrs[] = {
	{			/* This is a FIPS certified card using SCP01 security messaging. */
	 "3B:9F:95:81:31:FE:9F:00:66:46:53:05:10:00:11:71:df:00:00:00:6a:82:5e",
	 "FF:FF:FF:FF:FF:00:FF:FF:FF:FF:FF:FF:00:00:00:ff:00:ff:ff:00:00:00:00",
	 "FTCOS/ePass2003", SC_CARD_TYPE_ENTERSAFE_FTCOS_EPASS2003, 0, NULL},
	{NULL, NULL, NULL, 0, 0, NULL}
};

static struct sc_card_operations *iso_ops = NULL;
static struct sc_card_operations epass2003_ops;

static struct sc_card_driver epass2003_drv = {
	"epass2003",
	"epass2003",
	&epass2003_ops,
	NULL, 0, NULL
};

#define KEY_TYPE_AES	0x01	/* FIPS mode */
#define KEY_TYPE_DES	0x02	/* Non-FIPS mode */
static unsigned char g_smtype;	/* sm cryption algorithm type */

#define KEY_LEN_AES	16
#define KEY_LEN_DES	8
#define KEY_LEN_DES3	24
#define HASH_LEN	24

static unsigned char PIN_ID[2] = { ENTERSAFE_USER_PIN_ID, ENTERSAFE_SO_PIN_ID };
#define MAX_PIN_COUNTER						0x03

/*0x00:plain; 0x01:scp01 sm*/
#define SM_PLAIN				0x00
#define SM_SCP01				0x01
static unsigned char g_sm;	/* if perform sm or not */

static unsigned char g_init_key_enc[16] = 
{
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
	0x0D, 0x0E, 0x0F, 0x10
};
static unsigned char g_init_key_mac[16] = 
{
	0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
	0x0D, 0x0E, 0x0F, 0x10
};

static unsigned char g_random[8] =
{
	0xBF, 0xC3, 0x29, 0x11, 0xC7, 0x18, 0xC3, 0x40
};

static unsigned char g_sk_enc[16] = { 0 };	/* encrypt session key */
static unsigned char g_sk_mac[16] = { 0 };	/* mac session key */
static unsigned char g_icv_mac[16] = { 0 };	/* instruction counter vector(for sm) */

#define REVERSE_ORDER4(x)	(((unsigned long)x & 0xFF000000)>> 24  |		\
							 ((unsigned long)x & 0x00FF0000)>>  8  |		\
							 ((unsigned long)x & 0x0000FF00)<<  8  |		\
							 ((unsigned long)x & 0x000000FF)<< 24 )

static int openssl_enc( const EVP_CIPHER *cipher, const unsigned char *key, const unsigned char *iv, 
		const unsigned char* input, size_t length, unsigned char* output)
{
	int r = SC_ERROR_INTERNAL;
	EVP_CIPHER_CTX ctx;
	int outl = 0;
	int outl_tmp = 0;
	unsigned char iv_tmp[EVP_MAX_IV_LENGTH] = { 0 };
	memcpy(iv_tmp, iv, EVP_MAX_IV_LENGTH);
	EVP_CIPHER_CTX_init(&ctx);
	EVP_CIPHER_CTX_set_padding(&ctx, 0);
	EVP_EncryptInit_ex(&ctx, cipher, NULL, key, iv_tmp);
	if(!EVP_EncryptUpdate(&ctx, output, &outl, input, length))
	{
		goto out;
	}
	if (!EVP_EncryptFinal_ex(&ctx, output+outl, &outl_tmp))
	{
		goto out;
	}
	if (!EVP_CIPHER_CTX_cleanup(&ctx))
	{
		goto out;
	}
	r = SC_SUCCESS;
out:
	return r;
}

static int openssl_dec( const EVP_CIPHER *cipher, const unsigned char *key, const unsigned char *iv, 
		const unsigned char* input, size_t length, unsigned char* output)
{
	int r = SC_ERROR_INTERNAL;
	EVP_CIPHER_CTX ctx;
	int outl = 0;
	int outl_tmp = 0;
	unsigned char iv_tmp[EVP_MAX_IV_LENGTH] = { 0 };
	memcpy(iv_tmp, iv, EVP_MAX_IV_LENGTH);
	EVP_CIPHER_CTX_init(&ctx);
	EVP_CIPHER_CTX_set_padding(&ctx, 0);
	EVP_DecryptInit_ex(&ctx, cipher, NULL, key, iv_tmp);
	if(!EVP_DecryptUpdate(&ctx, output, &outl, input, length)) 
	{
		goto out;
	}
	if (!EVP_DecryptFinal_ex(&ctx, output+outl, &outl_tmp))
	{
		goto out;
	}
	if (!EVP_CIPHER_CTX_cleanup(&ctx)) 
	{
		goto out;
	}
	r = SC_SUCCESS;
out:
	return r;
}

static int aes128_encrypt_ecb(const unsigned char *key, int keysize,
		const unsigned char* input, size_t length, unsigned char* output)
{
	unsigned char iv[EVP_MAX_IV_LENGTH] = { 0 };
	return openssl_enc(EVP_aes_128_ecb(), key, iv, input, length, output);

}

static int aes128_encrypt_cbc( const unsigned char *key, int keysize, unsigned char iv[16], 
		const unsigned char *input, size_t length, unsigned char *output )
{
	return openssl_enc(EVP_aes_128_cbc(), key, iv, input, length, output);
}

static int aes128_decrypt_cbc( const unsigned char *key, int keysize, unsigned char iv[16], 
		const unsigned char *input, size_t length, unsigned char *output )
{
	return openssl_dec(EVP_aes_128_cbc(), key, iv, input, length, output);
}

static int des3_encrypt_ecb(const unsigned char *key, int keysize,
		const unsigned char* input, int length, unsigned char* output)
{
	unsigned char iv[EVP_MAX_IV_LENGTH] = { 0 };
	unsigned char bKey[24] = { 0 };
	if (keysize == 16)
	{
		memcpy(&bKey[0],key,16);
		memcpy(&bKey[16],key,8);
	}
	else
	{
		memcpy(&bKey[0],key,24);
	}
	return openssl_enc(EVP_des_ede3(), bKey, iv, input, length, output);
}

static int des3_encrypt_cbc( const unsigned char *key, int keysize, unsigned char iv[8], 
		const unsigned char *input, size_t length, unsigned char *output )
{
	unsigned char bKey[24] = { 0 };
	if (keysize == 16)
	{
		memcpy(&bKey[0],key,16);
		memcpy(&bKey[16],key,8);
	}
	else
	{
		memcpy(&bKey[0],key,24);
	}
	return openssl_enc(EVP_des_ede3_cbc(), bKey, iv, input, length, output);
}

static int des3_decrypt_cbc( const unsigned char *key, int keysize, unsigned char iv[8], 
		const unsigned char *input, size_t length, unsigned char *output )
{
	unsigned char bKey[24] = { 0 };
	if (keysize == 16)
	{
		memcpy(&bKey[0],key,16);
		memcpy(&bKey[16],key,8);
	}
	else
	{
		memcpy(&bKey[0],key,24);
	}
	return openssl_dec(EVP_des_ede3_cbc(), bKey, iv, input, length, output);
}

static int des_encrypt_cbc( const unsigned char *key, int keysize, unsigned char iv[8], 
		const unsigned char *input, size_t length, unsigned char *output )
{
	return openssl_enc(EVP_des_cbc(), key, iv, input, length, output);
}

static int des_decrypt_cbc( const unsigned char *key, int keysize, unsigned char iv[8], 
		const unsigned char *input, size_t length, unsigned char *output )
{
	return openssl_dec(EVP_des_cbc(), key, iv, input, length, output);
}

static int openssl_dig(const EVP_MD * digest,
		const unsigned char* input, size_t length, unsigned char* output)
{
	int r = SC_SUCCESS;
	EVP_MD_CTX ctx;
	int outl = 0;
	EVP_MD_CTX_init(&ctx);
	EVP_DigestInit_ex(&ctx, digest, NULL);
	if(!EVP_DigestUpdate(&ctx, input, length)) 
	{
		r = SC_ERROR_INTERNAL;
		goto out;
	}
	if (!EVP_DigestFinal_ex(&ctx, output, &outl))
	{
		r = SC_ERROR_INTERNAL;
		goto out;
	}
	if (!EVP_MD_CTX_cleanup(&ctx)) 
	{
		r = SC_ERROR_INTERNAL;
		goto out;
	}
out:
	return r;
}

static int sha1_digest(const unsigned char *input, size_t length, unsigned char *output )
{
	return openssl_dig(EVP_sha1(), input, length, output);
}

static int epass2003_transmit_apdu(struct sc_card *card, struct sc_apdu *apdu);

static int gen_init_key(struct sc_card *card, unsigned char* key_enc, unsigned char* key_mac, unsigned char* result, unsigned char key_type)
{
	int r;
	struct sc_apdu apdu;
	unsigned char data[256] = { 0 };
	unsigned char tmp_sm;
	unsigned long blocksize = 0;
	unsigned char cryptogram[256] = { 0 };	/* host cryptogram */
	unsigned char iv[16] = { 0 };
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x50, 0x00, 0x00);
	apdu.cla = 0x80;
	apdu.lc = apdu.datalen = sizeof(g_random);
	apdu.data = g_random;	/* host random */
	apdu.le = apdu.resplen = 28;
	apdu.resp = result;	/* card random is result[12~19] */

	tmp_sm = g_sm;
	g_sm = SM_PLAIN;
	r = epass2003_transmit_apdu(card, &apdu);
	g_sm = tmp_sm;
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU gen_init_key failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "gen_init_key failed");

	/* Step 1 - Generate Derivation data */
	memcpy(data, &result[16], 4);
	memcpy(&data[4], g_random, 4);
	memcpy(&data[8], &result[12], 4);
	memcpy(&data[12], &g_random[4], 4);

	/* Step 2,3 - Create S-ENC/S-MAC Session Key */
	if (KEY_TYPE_AES == key_type)
	{
		aes128_encrypt_ecb(key_enc, 16, data, 16, g_sk_enc);
		aes128_encrypt_ecb(key_mac, 16, data, 16, g_sk_mac);
	}
	else
	{
		des3_encrypt_ecb(key_enc, 16, data, 16, g_sk_enc);
		des3_encrypt_ecb(key_mac, 16, data, 16, g_sk_mac);
	}

	memcpy(data, g_random, 8);
	memcpy(&data[8], &result[12], 8);
	data[16] = 0x80;
	blocksize = (key_type == KEY_TYPE_AES ? 16 : 8);
	memset(&data[17], 0x00, blocksize - 1);

	/* calculate host cryptogram */
	if (KEY_TYPE_AES == key_type)
	{
		aes128_encrypt_cbc(g_sk_enc, 16, iv, data, 16+blocksize, cryptogram);	
	}
	else
	{
		des3_encrypt_cbc(g_sk_enc, 16, iv, data, 16+blocksize, cryptogram);
	}

	/* verify card cryptogram */
	if (0!=memcmp(&cryptogram[16], &result[20], 8))
	{
		return SC_ERROR_CARD_CMD_FAILED;
	}
	return SC_SUCCESS;
}

static int verify_init_key(struct sc_card *card, unsigned char* ran_key, unsigned char key_type)
{
	int r;
	struct sc_apdu apdu;
	unsigned long blocksize = (key_type == KEY_TYPE_AES ? 16 : 8);
	unsigned char data[256] = { 0 };
	unsigned char cryptogram[256] = { 0 };	/* host cryptogram */
	unsigned char iv[16] = { 0 };
	unsigned char mac[256] = { 0 };
	unsigned long i;
	unsigned char tmp_sm;
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	memcpy(data, ran_key, 8);
	memcpy(&data[8], g_random, 8);
	data[16] = 0x80;
	memset(&data[17], 0x00, blocksize - 1);
	memset(iv, 0, 16);

	/* calculate host cryptogram */
	if (KEY_TYPE_AES == key_type)
	{
		aes128_encrypt_cbc(g_sk_enc, 16, iv, data, 16+blocksize, cryptogram);
	}
	else
	{
		des3_encrypt_cbc(g_sk_enc, 16, iv, data, 16+blocksize, cryptogram);
	}

	memset(data, 0, sizeof(data));
	memcpy(data, "\x84\x82\x03\x00\x10", 5);
	memcpy(&data[5], &cryptogram[16], 8);
	memcpy(&data[13], "\x80\x00\x00", 3);

	/* calculate mac icv */
	memset(iv, 0x00, 16);
	if (KEY_TYPE_AES == key_type)
	{		
		aes128_encrypt_cbc(g_sk_mac, 16, iv, data, 16, mac);
		i = 0;
	}
	else
	{
		des3_encrypt_cbc(g_sk_mac, 16, iv, data, 16, mac);
		i = 8;
	}
	/* save mac icv */
	memset(g_icv_mac, 0x00, 16);
	memcpy(g_icv_mac, &mac[i], 8);

	/* verify host cryptogram */
	memcpy(data, &cryptogram[16], 8);
	memcpy(&data[8], &mac[i], 8);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x82, 0x03, 0x00);
	apdu.cla = 0x84;
	apdu.lc = apdu.datalen = 16;
	apdu.data = data;
	tmp_sm = g_sm;
	g_sm = SM_PLAIN;
	r = epass2003_transmit_apdu(card, &apdu);
	g_sm = tmp_sm;
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU verify_init_key failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "verify_init_key failed");
	return r;
}

static int mutural_auth(struct sc_card *card, unsigned char* key_enc, unsigned char* key_mac)
{
	int r;
	unsigned char result[256] = { 0 };
	unsigned char ran_key[8] = { 0 };
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	r = gen_init_key(card, key_enc, key_mac, result, g_smtype);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "gen_init_key failed");
	memcpy(ran_key, &result[12], 8);
	r = verify_init_key(card, ran_key, g_smtype);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "verify_init_key failed");
	return r;
}

int epass2003_refresh(struct sc_card *card)
{
	int r = 0;
	if (g_sm)
	{
		r = mutural_auth(card, g_init_key_enc, g_init_key_mac);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "mutural_auth failed");
	}
	return r;
}

/* Data(TLV)=0x87|L|0x01+Cipher */
static int construct_data_tlv(struct sc_apdu *apdu, unsigned char *apdu_buf,
		unsigned char* data_tlv, size_t* data_tlv_len, const unsigned char key_type)
{
	size_t block_size = (KEY_TYPE_AES == key_type ? 16 : 8);
	unsigned char pad[4096] = { 0 };
	size_t pad_len;
	size_t tlv_more;	/* increased tlv length */
	unsigned char iv[16] = { 0 };

	/* padding */
	apdu_buf[block_size] = 0x87;
	memcpy(pad, apdu->data, apdu->lc);
	pad[apdu->lc] = 0x80;
	if ((apdu->lc+1)%block_size)
	{
		pad_len = ((apdu->lc + 1) / block_size + 1) * block_size;
	}
	else
	{
		pad_len = apdu->lc + 1;
	}

	/* encode Lc' */
	if (pad_len>0x7E)
	{
		/* Lc' > 0x7E, use extended APDU */
		apdu_buf[block_size + 1] = 0x82;
		apdu_buf[block_size+2] = (unsigned char)((pad_len+1)/0x100);
		apdu_buf[block_size+3] = (unsigned char)((pad_len+1)%0x100);
		apdu_buf[block_size + 4] = 0x01;
		tlv_more = 5;
	}
	else
	{
		apdu_buf[block_size + 1] = (unsigned char)pad_len + 1;
		apdu_buf[block_size + 2] = 0x01;
		tlv_more = 3;
	}
	memcpy(data_tlv, &apdu_buf[block_size], tlv_more);

	/* encrypt Data */
	if (KEY_TYPE_AES == key_type)
	{	
		aes128_encrypt_cbc(g_sk_enc, 16, iv, pad, pad_len, apdu_buf+block_size+tlv_more);
	}
	else
	{
		des3_encrypt_cbc(g_sk_enc, 16, iv, pad, pad_len, apdu_buf+block_size+tlv_more);
	}
	memcpy(data_tlv + tlv_more, apdu_buf + block_size + tlv_more, pad_len);
	*data_tlv_len = tlv_more + pad_len;
	return 0;
}

/* Le(TLV)=0x97|L|Le */
static int construct_le_tlv(struct sc_apdu *apdu, unsigned char *apdu_buf,
		size_t data_tlv_len, unsigned char* le_tlv, size_t* le_tlv_len, const unsigned char key_type)
{
	size_t block_size = (KEY_TYPE_AES == key_type ? 16 : 8);
	*(apdu_buf + block_size + data_tlv_len) = 0x97;
	if (apdu->le > 0x7F)
	{
		/* Le' > 0x7E, use extended APDU */
		*(apdu_buf + block_size + data_tlv_len + 1) = 2;
		*(apdu_buf+block_size+data_tlv_len+2) = (unsigned char)(apdu->le/0x100);
		*(apdu_buf+block_size+data_tlv_len+3) = (unsigned char)(apdu->le%0x100);
		memcpy(le_tlv, apdu_buf + block_size + data_tlv_len, 4);
		*le_tlv_len = 4;
	}
	else
	{
		*(apdu_buf + block_size + data_tlv_len + 1) = 1;
		*(apdu_buf+block_size+data_tlv_len+2) = (unsigned char)apdu->le;
		memcpy(le_tlv, apdu_buf + block_size + data_tlv_len, 3);
		*le_tlv_len = 3;
	}
	return 0;
}

/* MAC(TLV)=0x8e|0x08|MAC */
static int construct_mac_tlv(unsigned char *apdu_buf, size_t data_tlv_len,
		size_t le_tlv_len, unsigned char* mac_tlv, size_t* mac_tlv_len, const unsigned char key_type)
{
	size_t block_size = (KEY_TYPE_AES == key_type ? 16 : 8);
	unsigned char mac[4096] = { 0 };
	size_t mac_len;
	unsigned char icv[16] = { 0 };
	size_t i = (KEY_TYPE_AES == key_type ? 15 : 7);

	if (0==data_tlv_len && 0==le_tlv_len)
	{
		mac_len = block_size;
	}
	else
	{
		/* padding */
		*(apdu_buf + block_size + data_tlv_len + le_tlv_len) = 0x80;
		if ((data_tlv_len+le_tlv_len+1)%block_size)
		{
			mac_len = (((data_tlv_len+le_tlv_len+1)/block_size)+1)*block_size+block_size;
		}
		else
		{
			mac_len = data_tlv_len + le_tlv_len + 1 + block_size;
		}
		memset((apdu_buf+block_size+data_tlv_len+le_tlv_len+1), 0, (mac_len - (data_tlv_len+le_tlv_len+1)));				
	}

	/* increase icv */
	for(; i>=0; i--)
	{
		if (g_icv_mac[i] == 0xff)
		{
			g_icv_mac[i] = 0;
		}
		else
		{
			g_icv_mac[i]++;
			break;
		}
	}

	/* calculate MAC */
	memset(icv, 0, sizeof(icv));
	memcpy(icv, g_icv_mac, 16);
	if (KEY_TYPE_AES == key_type)
	{
		aes128_encrypt_cbc(g_sk_mac, 16, icv, apdu_buf, mac_len, mac);
		memcpy(mac_tlv + 2, &mac[mac_len - 16], 8);
	}
	else
	{
		unsigned char iv[8] = { 0 };
		unsigned char tmp[8] = { 0 };
		des_encrypt_cbc(g_sk_mac, 8, icv, apdu_buf, mac_len, mac);
		des_decrypt_cbc(&g_sk_mac[8], 8, iv, &mac[mac_len - 8], 8, tmp);
		memset(iv, 0x00, 8);
		des_encrypt_cbc(g_sk_mac, 8, iv, tmp, 8, mac_tlv + 2);
	}
	*mac_tlv_len = 2 + 8;
	return 0;
}

static size_t calc_le(size_t le)
{
	size_t le_new = 0;
	size_t resp_len = 0;
	size_t sw_len = 4;	/* T 1 L 1 V 2 */
	size_t mac_len = 10;	/* T 1 L 1 V 8 */
	size_t mod = 16;
	/* padding first */
	resp_len = 1 + ((le + (mod - 1)) / mod) * mod;

	if( 0x7f < resp_len )
	{
		resp_len += 0;

	} else if( 0x7f <= resp_len && resp_len < 0xff)
	{
		resp_len += 1;
	}
	else if( 0xff <= resp_len)
	{
		resp_len += 2;
	}
	resp_len += 2;		/* +T+L */
	le_new = resp_len + sw_len + mac_len;
	return le_new;
}

/* According to GlobalPlatform Card Specification's SCP01
 * encode APDU from
 * CLA INS P1 P2 [Lc] Data [Le] 
 * to
 * CLA INS P1 P2 Lc' Data' [Le]
 * where
 * Data'=Data(TLV)+Le(TLV)+MAC(TLV) */
static int encode_apdu(struct sc_apdu *plain, struct sc_apdu *sm, unsigned char *apdu_buf, size_t *apdu_buf_len)
{
	size_t block_size = (KEY_TYPE_DES == g_smtype ? 16 : 8);
	unsigned char dataTLV[4096] = { 0 };
	size_t data_tlv_len = 0;
	unsigned char le_tlv[256] = { 0 };
	size_t le_tlv_len = 0;
	size_t mac_tlv_len = 10;
	size_t tmp_lc;
	size_t tmp_le;
	unsigned char mac_tlv[256] = {0};
	mac_tlv[0] = 0x8E;
	mac_tlv[1] = 8;
	/* size_t plain_le = 0; */

	sm->cse = SC_APDU_CASE_4_SHORT;
	apdu_buf[0] = (unsigned char)plain->cla;
	apdu_buf[1] = (unsigned char)plain->ins;
	apdu_buf[2] = (unsigned char)plain->p1;
	apdu_buf[3] = (unsigned char)plain->p2;

	/* plain_le = plain->le; */

	/* padding */
	apdu_buf[4] = 0x80;
	memset(&apdu_buf[5], 0x00, block_size - 5);

	/* Data -> Data' */
	if(plain->lc != 0)
	{			
		if (0 != construct_data_tlv(plain, apdu_buf, dataTLV, &data_tlv_len, g_smtype))
		{
			return -1;
		}
	}
	if(plain->le != 0 ||(plain->le == 0 && plain->resplen != 0))
	{
		if (0 != construct_le_tlv(plain, apdu_buf, data_tlv_len, le_tlv, &le_tlv_len, g_smtype))
		{
			return -1;
		}
	}
	if (0 != construct_mac_tlv(apdu_buf, data_tlv_len, le_tlv_len, mac_tlv, &mac_tlv_len, g_smtype))
	{
		return -1;
	}
	memset(apdu_buf + 4, 0, *apdu_buf_len - 4);
	sm->lc = sm->datalen = data_tlv_len + le_tlv_len + mac_tlv_len;
	if (sm->lc > 0xFF)
	{
		sm->cse = SC_APDU_CASE_4_EXT;
		apdu_buf[4] = (unsigned char)((sm->lc) / 0x10000);
		apdu_buf[5] = (unsigned char)(((sm->lc) / 0x100) % 0x100);
		apdu_buf[6] = (unsigned char)((sm->lc) % 0x100);
		tmp_lc = 3;
	} 
	else
	{
		apdu_buf[4] = (unsigned char)sm->lc;
		tmp_lc = 1;
	}
	memcpy(apdu_buf + 4 + tmp_lc, dataTLV, data_tlv_len);
	memcpy(apdu_buf + 4 + tmp_lc + data_tlv_len, le_tlv, le_tlv_len);
	memcpy(apdu_buf+4+tmp_lc+data_tlv_len+le_tlv_len, mac_tlv, mac_tlv_len);
	memcpy((unsigned char *)sm->data, apdu_buf + 4 + tmp_lc, sm->datalen);
	*apdu_buf_len = 0;
	if (4 == le_tlv_len)
	{
		sm->cse = SC_APDU_CASE_4_EXT;
		*(apdu_buf+4+tmp_lc+sm->lc)= (unsigned char)(plain->le/0x100);
		*(apdu_buf+4+tmp_lc+sm->lc+1) = (unsigned char)(plain->le%0x100);
		tmp_le = 2;
	}
	else if (3 == le_tlv_len)
	{
		*(apdu_buf + 4 + tmp_lc + sm->lc) = (unsigned char)plain->le;
		tmp_le = 1;
	}
	*apdu_buf_len += 4+tmp_lc+data_tlv_len+le_tlv_len+mac_tlv_len+tmp_le;
	/* sm->le = calc_le(plain_le); */
	return 0;
}

static int epass2003_sm_wrap_apdu(struct sc_card *card,
				  struct sc_apdu *plain, struct sc_apdu *sm)
{
	int r;
	unsigned char buf[4096] = { 0 };	/* APDU buffer */
	size_t buf_len = sizeof(buf);
	size_t ssize = 0;
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	if (g_sm)
	{
		plain->cla |= 0x0C;
	}

	sm->cse = plain->cse;
	sm->cla = plain->cla;
	sm->ins = plain->ins;
	sm->p1 = plain->p1;
	sm->p2 = plain->p2;
	sm->lc = plain->lc;
	sm->le = plain->le;
	sm->control = plain->control;
	sm->flags = plain->flags;

	switch(sm->cla & 0x0C)
	{
	case 0x00:
	case 0x04:
		{
			sm->datalen = plain->datalen;
			sm->data = plain->data;
			sm->resplen = plain->resplen;
			sm->resp = plain->resp;
		}
		break;
	case 0x0C:
		{
			memset(buf, 0, sizeof(buf));
				if (0 != encode_apdu(plain, sm, buf, &buf_len))
				{
				return SC_ERROR_CARD_CMD_FAILED;
			}
		}
		break;
	default:
		return SC_ERROR_INCORRECT_PARAMETERS;
	}
	return SC_SUCCESS;
}

/* According to GlobalPlatform Card Specification's SCP01
 * decrypt APDU response from
 * ResponseData' SW1 SW2 
 * to
 * ResponseData SW1 SW2
 * where
 * ResponseData'=Data(TLV)+SW12(TLV)+MAC(TLV)
 * where
 * Data(TLV)=0x87|L|Cipher 
 * SW12(TLV)=0x99|0x02|SW1+SW2
 * MAC(TLV)=0x8e|0x08|MAC */
static int decrypt_response(unsigned char* in, unsigned char* out, size_t* out_len)
{
	size_t in_len;
	size_t i;
	unsigned char iv[16] = { 0 };
	unsigned char plaintext[4096] = { 0 };

	if (in[0] == 0x99)
	{
		/* no cipher */
		return 0;
	}

	/* parse cipher length */
	if (0x01==in[2] && 0x82!=in[1])
	{
		in_len = in[1];
		i = 3;
	}
	else if (0x01==in[3] && 0x81==in[1])
	{
		in_len = in[2];
		i = 4;
	}
	else if (0x01==in[4] && 0x82==in[1])
	{
		in_len = in[2] * 0x100;
		in_len += in[3];
		i = 5;
	}
	else
	{
		return -1;
	}

	/* decrypt */
	if (KEY_TYPE_AES == g_smtype)
	{
		aes128_decrypt_cbc(g_sk_enc, 16, iv, &in[i], in_len-1, plaintext);
	}
	else
	{
		des3_decrypt_cbc(g_sk_enc, 16, iv, &in[i], in_len-1, plaintext);
	}

	/* unpadding */
	while(0x80!=plaintext[in_len-2] && (in_len-2>0))
	{
		in_len--;
	}
	if (2 == in_len)
	{
		return -1;
	}
	memcpy(out, plaintext, in_len - 2);
	*out_len = in_len - 2;
	return 0;
}

static int epass2003_sm_unwrap_apdu(struct sc_card *card,
				    struct sc_apdu *sm, struct sc_apdu *plain)
{
	int r;
	size_t len = 0;
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	r = sc_check_sw(card, sm->sw1, sm->sw2);
	if (r == SC_SUCCESS)
	{
		if (g_sm)
		{
			if (0 != decrypt_response(sm->resp, plain->resp, &len))
			{
				return SC_ERROR_CARD_CMD_FAILED;
			}
			plain->resplen = len;
		}
		else
		{
			memcpy(plain->resp, sm->resp, sm->resplen);
			plain->resplen = sm->resplen;
		}
	}
	plain->sw1 = sm->sw1;
	plain->sw2 = sm->sw2;

	return SC_SUCCESS;
}

static int epass2003_transmit_apdu(struct sc_card *card, struct sc_apdu *apdu)
{
	int r;
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	r = sc_transmit_apdu(card, apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	return r;
}

static int get_data(struct sc_card *card, unsigned char type, unsigned char* data, size_t datalen)
{
	int r;
	struct sc_apdu apdu;
	unsigned char resp[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	size_t resplen = SC_MAX_APDU_BUFFER_SIZE;
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0xca, 0x01, type);
	apdu.resp = resp;
	apdu.le = 0;
	apdu.resplen = resplen;
	if (0x86 == type)
	{
		/* No SM temporarily */
		unsigned char tmp_sm = g_sm;
		g_sm = SM_PLAIN;
		r = sc_transmit_apdu(card, &apdu);
		g_sm = tmp_sm;
	}
	else
	{
		r = sc_transmit_apdu(card, &apdu);
	}
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU get_data failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "get_data failed");
	memcpy(data, resp, datalen);
	return r;
}

/* card driver functions */

static int epass2003_match_card(struct sc_card *card)
{
	int i;
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	i = _sc_match_atr(card, epass2003_atrs, &card->type);
	if (i < 0)
		return 0;

	return 1;
}

static int epass2003_init(struct sc_card *card)
{
	unsigned int flags;
	unsigned char data[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	size_t datalen = SC_MAX_APDU_BUFFER_SIZE;
	unsigned char size[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	size_t sizelen = SC_MAX_APDU_BUFFER_SIZE;
	unsigned char tmp_sm = 0x00;
	struct sc_apdu apdu;
	unsigned char rbuf[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	unsigned char random[16] = { 0 };
	int r;


	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	card->name = "epass2003";
	card->cla = 0x00;
	card->drv_data = NULL;
	card->ctx->use_sm = 1;

	g_sm = SM_SCP01;
	/* g_sm = SM_PLAIN; */

	/* decide FIPS/Non-FIPS mode */
	if (SC_SUCCESS != get_data(card, 0x86, data, datalen))
	{
		return SC_ERROR_CARD_CMD_FAILED;
	}
	if (0x01 == data[2])
	{
		g_smtype = KEY_TYPE_AES;
	}
	else
	{
		g_smtype = KEY_TYPE_DES;
	}

	/* mutural anthentication */
	epass2003_refresh(card);

	flags = SC_ALGORITHM_ONBOARD_KEY_GEN
		| SC_ALGORITHM_RSA_RAW
		| SC_ALGORITHM_RSA_HASH_NONE;

	_sc_card_add_rsa_alg(card, 512, flags, 0x10001);
	_sc_card_add_rsa_alg(card, 768, flags, 0x10001);
	_sc_card_add_rsa_alg(card, 1024, flags, 0x10001);
	_sc_card_add_rsa_alg(card, 2048, flags, 0x10001);

	card->caps = SC_CARD_CAP_RNG |
		SC_CARD_CAP_APDU_EXT;

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

/* COS implement SFI as lower 5 bits of FID, and not allow same SFI at the
 * same DF, so use hook functions to increase/decrease FID by 0x20 */
static int epass2003_hook_path(struct sc_path *path, int inc)
{
	u8 fid_h = path->value[path->len - 2];
	u8 fid_l = path->value[path->len - 1];
	switch ( fid_h )
	{
	case 0x29:
	case 0x30:
	case 0x31:
	case 0x32:
	case 0x33:
	case 0x34:
			if( inc )
			{
			fid_l = fid_l * FID_STEP;
			}
			else
			{
			fid_l = fid_l / FID_STEP;
		}
		path->value[path->len - 1] = fid_l;
		return 1;
		break;
	default:
		break;
	}
	return 0;
}

static void epass2003_hook_file(struct sc_file *file, int inc)
{
	int fidl = file->id & 0xff;
	int fidh = file->id & 0xff00;
	if( epass2003_hook_path(&file->path, inc) )
	{
		if( inc )
		{
			file->id = fidh + fidl * FID_STEP;
		}
		else
		{
			file->id = fidh + fidl / FID_STEP;
		}
	}
}

static int epass2003_select_fid_(struct sc_card *card,
			       sc_path_t *in_path,
			       sc_file_t **file_out)
{
	sc_context_t *ctx;
	struct sc_apdu apdu;
	u8 buf[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	u8 pathbuf[SC_MAX_PATH_SIZE], *path = pathbuf;
	int r, pathlen;
	sc_file_t *file = NULL;

	ctx = card->ctx;
	epass2003_hook_path(in_path, 1);
	memcpy(path, in_path->value, in_path->len);
	pathlen = in_path->len;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0xA4, 0x00, 0x00);

	switch (in_path->type) {
	case SC_PATH_TYPE_FILE_ID:
		apdu.p1 = 0;
		if (pathlen != 2)
			return SC_ERROR_INVALID_ARGUMENTS;
		break;
	default:
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INVALID_ARGUMENTS);
	}
	apdu.p2 = 0;		/* first record, return FCI */
	apdu.lc = pathlen;
	apdu.data = path;
	apdu.datalen = pathlen;

	if (file_out != NULL) {
		apdu.resp = buf;
		apdu.resplen = sizeof(buf);
		apdu.le = 0;
	} else
		apdu.cse = (apdu.lc == 0) ? SC_APDU_CASE_1 : SC_APDU_CASE_3_SHORT;

	if (path[0] == 0x29) {	/* TODO:0x29 accords with FID prefix in profile  */
		/* Not allowed to select prvate key file, so fake fci. */
		/* 62 16 82 02 11 00 83 02 29 00 85 02 08 00 86 08 FF 90 90 90 FF FF FF FF */
		apdu.resplen = 0x18;
		memcpy(apdu.resp, "\x6f\x16\x82\x02\x11\x00\x83\x02\x29\x00\x85\x02\x08\x00\x86\x08\xff\x90\x90\x90\xff\xff\xff\xff", apdu.resplen);
		apdu.resp[9] = path[1];
		apdu.sw1 = 0x90;
		apdu.sw2 = 0x00;
	}
	else {
		r = sc_transmit_apdu(card, &apdu);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	}
	if (file_out == NULL) {
		if (apdu.sw1 == 0x61)
			SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, 0);
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, sc_check_sw(card, apdu.sw1, apdu.sw2));
	}

	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	if (r)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, r);

	if (apdu.resplen < 2)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_UNKNOWN_DATA_RECEIVED);
	switch (apdu.resp[0]) {
	case 0x6F:
		file = sc_file_new();
		if (file == NULL)
			SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_OUT_OF_MEMORY);
		file->path = *in_path;
		if (card->ops->process_fci == NULL) {
			sc_file_free(file);
			SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_NOT_SUPPORTED);
		}
		if ((size_t) apdu.resp[1] + 2 <= apdu.resplen)
			card->ops->process_fci(card, file, apdu.resp+2, apdu.resp[1]);
		epass2003_hook_file(file, 0);
		*file_out = file;
		break;
	case 0x00:		/* proprietary coding */
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_UNKNOWN_DATA_RECEIVED);
		break;
	default:
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_UNKNOWN_DATA_RECEIVED);
	}
	return 0;
}

static int epass2003_select_fid(struct sc_card *card,
				unsigned int id_hi, unsigned int id_lo,
				sc_file_t ** file_out)
{
	int r;
	sc_file_t *file = 0;
	sc_path_t path;

	path.type = SC_PATH_TYPE_FILE_ID;
	path.value[0] = id_hi;
	path.value[1] = id_lo;
	path.len = 2;

	r = epass2003_select_fid_(card, &path, &file);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");

	/* update cache */
	if (file->type == SC_FILE_TYPE_DF) {
		card->cache.current_path.type = SC_PATH_TYPE_PATH;
		card->cache.current_path.value[0] = 0x3f;
		card->cache.current_path.value[1] = 0x00;
		if (id_hi == 0x3f && id_lo == 0x00) {
			card->cache.current_path.len = 2;
		} else {
			card->cache.current_path.len = 4;
			card->cache.current_path.value[2] = id_hi;
			card->cache.current_path.value[3] = id_lo;
		}
	}

	if (file_out)
		*file_out = file;

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int epass2003_select_aid(struct sc_card *card,
				const sc_path_t * in_path,
				sc_file_t ** file_out)
{
	int r = 0;

	if (card->cache.valid
	    && card->cache.current_path.type == SC_PATH_TYPE_DF_NAME
	    && card->cache.current_path.len == in_path->len
		&& memcmp(card->cache.current_path.value, in_path->value, in_path->len)==0 )
	{
		 if(file_out)
		 {
			*file_out = sc_file_new();
			if (!file_out)
				   SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_OUT_OF_MEMORY);
		}
	}
	else
	{
		r = iso_ops->select_file(card, in_path, file_out);
		 SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");

		/* update cache */
		card->cache.current_path.type = SC_PATH_TYPE_DF_NAME;
		card->cache.current_path.len = in_path->len;
		 memcpy(card->cache.current_path.value,in_path->value,in_path->len);
	}
	if (file_out) {
		sc_file_t *file = *file_out;

		file->type = SC_FILE_TYPE_DF;
		file->ef_structure = SC_FILE_EF_UNKNOWN;
		file->path.len = 0;
		file->size = 0;
		/* AID */
		memcpy(file->name, in_path->value, in_path->len);
		file->namelen = in_path->len;
		file->id = 0x0000;
	}
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, r);
}

static int epass2003_select_file(struct sc_card *card,
				 const sc_path_t * in_path,
				 sc_file_t ** file_out);

static int epass2003_select_path(struct sc_card *card,
				 const u8 pathbuf[16], const size_t len,
				 sc_file_t ** file_out)
{
	u8 n_pathbuf[SC_MAX_PATH_SIZE];
	const u8 *path = pathbuf;
	size_t pathlen = len;
	int bMatch = -1;
	unsigned int i;
	int r;

	if (pathlen % 2 != 0 || pathlen > 6 || pathlen <= 0)
		  SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INVALID_ARGUMENTS);

	/* if pathlen == 6 then the first FID must be MF (== 3F00) */
	if (pathlen == 6 && (path[0] != 0x3f || path[1] != 0x00))
		  SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INVALID_ARGUMENTS);

	/* unify path (the first FID should be MF) */
	 if (path[0] != 0x3f || path[1] != 0x00)
	 {
		n_pathbuf[0] = 0x3f;
		n_pathbuf[1] = 0x00;
		for (i = 0; i < pathlen; i++)
			n_pathbuf[i + 2] = pathbuf[i];
		path = n_pathbuf;
		pathlen += 2;
	}

	/* check current working directory */
	if (card->cache.valid
	    && card->cache.current_path.type == SC_PATH_TYPE_PATH
	    && card->cache.current_path.len >= 2
		 && card->cache.current_path.len <= pathlen )
	 {
		bMatch = 0;
		for (i = 0; i < card->cache.current_path.len; i += 2)
			if (card->cache.current_path.value[i] == path[i]
				   && card->cache.current_path.value[i+1] == path[i+1] )
				bMatch += 2;
	}

	 if ( card->cache.valid && bMatch > 2 )
	 {
		  if ( pathlen - bMatch == 2 )
		  {
			/* we are in the rigth directory */
			   return epass2003_select_fid(card, path[bMatch], path[bMatch+1], file_out);
		  }
		  else if ( pathlen - bMatch > 2 )
		  {
			/* two more steps to go */
			sc_path_t new_path;

			/* first step: change directory */
			   r = epass2003_select_fid(card, path[bMatch], path[bMatch+1], NULL);
			   SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "SELECT FILE (DF-ID) failed");

			new_path.type = SC_PATH_TYPE_PATH;
			new_path.len = pathlen - bMatch - 2;
			   memcpy(new_path.value, &(path[bMatch+2]), new_path.len);
			/* final step: select file */
			return epass2003_select_file(card, &new_path, file_out);
		  }
		  else /* if (bMatch - pathlen == 0) */
		  {
			/* done: we are already in the
			 * requested directory */
			   sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
				"cache hit\n");
			/* copy file info (if necessary) */
			if (file_out) {
				sc_file_t *file = sc_file_new();
				if (!file)
						 SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_OUT_OF_MEMORY);
					file->id = (path[pathlen-2] << 8) +
						 path[pathlen-1];
				file->path = card->cache.current_path;
				file->type = SC_FILE_TYPE_DF;
				file->ef_structure = SC_FILE_EF_UNKNOWN;
				file->size = 0;
				file->namelen = 0;
				file->magic = SC_FILE_MAGIC;
				*file_out = file;
			}
			/* nothing left to do */
			return SC_SUCCESS;
		}
	 }
	 else
	 {
		/* no usable cache */
		  for ( i=0; i<pathlen-2; i+=2 )
		  {
			   r = epass2003_select_fid(card, path[i], path[i+1], NULL);
			   SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "SELECT FILE (DF-ID) failed");
		}
		  return epass2003_select_fid(card, path[pathlen-2], path[pathlen-1], file_out);
	}
}

static int epass2003_select_file(struct sc_card *card,
				 const sc_path_t * in_path,
				 sc_file_t ** file_out)
{
	int r;
	char pbuf[SC_MAX_PATH_STRING_SIZE];
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);


	r = sc_path_print(pbuf, sizeof(pbuf), &card->cache.current_path);
	if (r != SC_SUCCESS)
		pbuf[0] = '\0';

	sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
		 "current path (%s, %s): %s (len: %u)\n",
		   (card->cache.current_path.type==SC_PATH_TYPE_DF_NAME?"aid":"path"),
		 (card->cache.valid ? "valid" : "invalid"), pbuf,
		 card->cache.current_path.len);

	 switch(in_path->type)
	 {
	case SC_PATH_TYPE_FILE_ID:
		if (in_path->len != 2)
			   SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE,SC_ERROR_INVALID_ARGUMENTS);
		  return epass2003_select_fid(card,in_path->value[0],in_path->value[1], file_out);
	case SC_PATH_TYPE_DF_NAME:
		return epass2003_select_aid(card, in_path, file_out);
	case SC_PATH_TYPE_PATH:
		  return epass2003_select_path(card,in_path->value,in_path->len,file_out);
	default:
		  SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INVALID_ARGUMENTS);
	}
}

static int epass2003_set_security_env(struct sc_card *card,
				    const sc_security_env_t *env,
				    int se_num)
{
	struct sc_apdu apdu;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	u8 *p;
	unsigned short fid = 0;
	int r, locked = 0;

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0x41, 0);
	switch (env->operation) {
	case SC_SEC_OPERATION_DECIPHER:
		apdu.p2 = 0xB8;
		break;
	case SC_SEC_OPERATION_SIGN:
		apdu.p2 = 0xB8;
		break;
	default:
		return SC_ERROR_INVALID_ARGUMENTS;
	}
	p = sbuf;
	*p++ = 0x80;		/* algorithm reference */
	*p++ = 0x01;
	*p++ = 0x84;

	*p++ = 0x81;
	*p++ = 0x02;

	fid = 0x2900;
	fid += (unsigned short)(0x20 * (env->key_ref[0] & 0xff));
	*p++ = fid >> 8;
	*p++ = fid & 0xff;
	r = p - sbuf;
	apdu.lc = r;
	apdu.datalen = r;
	apdu.data = sbuf;
	if (se_num > 0) {
		r = sc_lock(card);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "sc_lock() failed");
		locked = 1;
	}
	if (apdu.datalen != 0) {
		r = sc_transmit_apdu(card, &apdu);
		if (r) {
			sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
				 "%s: APDU transmit failed", sc_strerror(r));
			goto err;
		}
		r = sc_check_sw(card, apdu.sw1, apdu.sw2);
		if (r) {
			sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL,
				 "%s: Card returned error", sc_strerror(r));
			goto err;
		}
	}
	if (se_num <= 0)
		return 0;
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x22, 0xF2, se_num);
	r = sc_transmit_apdu(card, &apdu);
	sc_unlock(card);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
err:
	if (locked)
		sc_unlock(card);
	return r;
}

static int epass2003_restore_security_env(struct sc_card *card, int se_num)
{
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_NORMAL);
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int epass2003_decipher(struct sc_card *card,
			      const u8 * data, size_t datalen,
			      u8 * out, size_t outlen)
{
	int r;
	struct sc_apdu apdu;
	u8 rbuf[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE] = { 0 };

	if (datalen > 255)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INVALID_ARGUMENTS);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_4_SHORT, 0x2A, 0x80, 0x86);
	apdu.resp = rbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.le = 256;

	memcpy(sbuf, data, datalen);
	apdu.data = sbuf;
	apdu.lc = datalen;
	apdu.datalen = datalen;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	if (apdu.sw1 == 0x90 && apdu.sw2 == 0x00) {
		size_t len = apdu.resplen > outlen ? outlen : apdu.resplen;
		memcpy(out, apdu.resp, len);
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, len);
	}
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, sc_check_sw(card, apdu.sw1, apdu.sw2));
}

static int 
acl_to_ac_byte(struct sc_card *card, const struct sc_acl_entry *e)
{
	unsigned key_ref;

	if (e == NULL)
		return SC_ERROR_OBJECT_NOT_FOUND;

	switch (e->method) {
	case SC_AC_NONE:
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, EPASS2003_AC_MAC_NOLESS|EPASS2003_AC_EVERYONE);
	case SC_AC_NEVER:
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, EPASS2003_AC_MAC_NOLESS|EPASS2003_AC_NOONE);
	default:
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, EPASS2003_AC_MAC_NOLESS|EPASS2003_AC_USER);
	}
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INCORRECT_PARAMETERS);
}

static int epass2003_process_fci(struct sc_card *card, sc_file_t * file,
				 const u8 * buf, size_t buflen)
{
	sc_context_t *ctx = card->ctx;
	size_t taglen, len = buflen;
	const u8 *tag = NULL, *p = buf;

	sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "processing FCI bytes");
	tag = sc_asn1_find_tag(ctx, p, len, 0x83, &taglen);
	if (tag != NULL && taglen == 2) {
		file->id = (tag[0] << 8) | tag[1];
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL,
			 "  file identifier: 0x%02X%02X", tag[0], tag[1]);
	}
	tag = sc_asn1_find_tag(ctx, p, len, 0x80, &taglen);
	if (tag != NULL && taglen > 0 && taglen < 3) {
		file->size = tag[0];
		if (taglen == 2)
			file->size = (file->size << 8) + tag[1];
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "  bytes in file: %d", file->size);
	}
	if (tag == NULL) {
		tag = sc_asn1_find_tag(ctx, p, len, 0x81, &taglen);
		if (tag != NULL && taglen >= 2) {
			int bytes = (tag[0] << 8) + tag[1];
			sc_debug(ctx, SC_LOG_DEBUG_NORMAL,
				 "  bytes in file: %d", bytes);
			file->size = bytes;
		}
	}
	tag = sc_asn1_find_tag(ctx, p, len, 0x82, &taglen);
	if (tag != NULL) {
		if (taglen > 0) {
			unsigned char byte = tag[0];
			const char *type;

			if (byte == 0x38) {
				type = "DF";
				file->type = SC_FILE_TYPE_DF;
			}
			else if( 0x01 <= byte && byte <= 0x07) {
				type = "working EF";
				file->type = SC_FILE_TYPE_WORKING_EF;
				switch (byte) {
				case 0x01:
						file->ef_structure = SC_FILE_EF_TRANSPARENT;
					break;
				case 0x02:
						file->ef_structure = SC_FILE_EF_LINEAR_FIXED;
					break;
				case 0x03:
					break;
				case 0x04:
						file->ef_structure = SC_FILE_EF_LINEAR_FIXED;
					break;
				case 0x05:
					break;
				case 0x06:
					break;
				case 0x07:
					break;
				default:
					break;
				}

			}
			else if( 0x10 == byte) {
				type = "BSO";
				file->type = SC_FILE_TYPE_BSO;
			}
			else if( 0x11 <= byte) {
				type = "internal EF";
				file->type = SC_FILE_TYPE_INTERNAL_EF;
				switch (byte) {
				case 0x11:
					break;
				case 0x12:
					break;
				default:
					break;
				}
			}
			else {
				type = "unknown";
				file->type = SC_FILE_TYPE_INTERNAL_EF;

			}
			sc_debug(ctx, SC_LOG_DEBUG_NORMAL,
				"  type: %s", type);
			sc_debug(ctx, SC_LOG_DEBUG_NORMAL,
				 "  EF structure: %d", byte);
		}
	}
	tag = sc_asn1_find_tag(ctx, p, len, 0x84, &taglen);
	if (tag != NULL && taglen > 0 && taglen <= 16) {
		char tbuf[128];
		memcpy(file->name, tag, taglen);
		file->namelen = taglen;

		sc_hex_dump(ctx, SC_LOG_DEBUG_NORMAL,
			    file->name, file->namelen, tbuf, sizeof(tbuf));
		sc_debug(ctx, SC_LOG_DEBUG_NORMAL, "  File name: %s", tbuf);
		if (!file->type)
			file->type = SC_FILE_TYPE_DF;
	}
	tag = sc_asn1_find_tag(ctx, p, len, 0x85, &taglen);
	if (tag != NULL && taglen) {
		sc_file_set_prop_attr(file, tag, taglen);
	} else
		file->prop_attr_len = 0;
	tag = sc_asn1_find_tag(ctx, p, len, 0xA5, &taglen);
	if (tag != NULL && taglen) {
		sc_file_set_prop_attr(file, tag, taglen);
	}
	tag = sc_asn1_find_tag(ctx, p, len, 0x86, &taglen);
	if (tag != NULL && taglen) {
		sc_file_set_sec_attr(file, tag, taglen);
	}
	tag = sc_asn1_find_tag(ctx, p, len, 0x8A, &taglen);
	if (tag != NULL && taglen == 1) {
		if (tag[0] == 0x01)
			file->status = SC_FILE_STATUS_CREATION;
		else if (tag[0] == 0x07 || tag[0] == 0x05)
			file->status = SC_FILE_STATUS_ACTIVATED;
		else if (tag[0] == 0x06 || tag[0] == 0x04)
			file->status = SC_FILE_STATUS_INVALIDATED;
	}
	file->magic = SC_FILE_MAGIC;

	return 0;
}

static int epass2003_construct_fci(struct sc_card *card, const sc_file_t * file,
				   u8 * out, size_t * outlen)
{
	u8 *p = out;
	u8 buf[64];
	unsigned char  ops[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	int ii, rv;

	if (*outlen < 2)
		return SC_ERROR_BUFFER_TOO_SMALL;
	*p++ = 0x62;
	p++;
	if (file->type == SC_FILE_TYPE_WORKING_EF) {
		if (file->ef_structure == SC_FILE_EF_TRANSPARENT) {
			buf[0] = (file->size >> 8) & 0xFF;
			buf[1] = file->size & 0xFF;
		sc_asn1_put_tag(0x80, buf, 2, p, *outlen - (p - out), &p);
		}
	}
	if (file->type == SC_FILE_TYPE_DF) {
		buf[0] = 0x38;
		buf[1] = 0x00;
		sc_asn1_put_tag(0x82, buf, 2, p, *outlen - (p - out), &p);
	}
	else if( file->type == SC_FILE_TYPE_WORKING_EF )
	{
		buf[0] = file->ef_structure & 7;
		if( file->ef_structure == SC_FILE_EF_TRANSPARENT)
		{
			buf[1] = 0x00;
			sc_asn1_put_tag(0x82, buf, 2, p, *outlen - (p - out), &p);
		}
		else if ( file->ef_structure == SC_FILE_EF_LINEAR_FIXED ||
				file->ef_structure == SC_FILE_EF_LINEAR_VARIABLE) {
			buf[1] = 0x00;
			buf[2] = 0x00;
			buf[3] = 0x40;	/* record length */
			buf[4] = 0x00;	/* record count */
			sc_asn1_put_tag(0x82, buf, 5, p, *outlen - (p - out), &p);
		}
		else {
			return SC_ERROR_NOT_SUPPORTED;
		}

	} 
	else if( file->type == SC_FILE_TYPE_INTERNAL_EF ) {
		if( file->ef_structure == SC_CARDCTL_OBERTHUR_KEY_RSA_CRT )
		{
			buf[0] = 0x11;
			buf[1] = 0x00;
		}
		else if( file->ef_structure == SC_CARDCTL_OBERTHUR_KEY_RSA_PUBLIC )
		{
			buf[0] = 0x12;
			buf[1] = 0x00;
		}
		else {
			return SC_ERROR_NOT_SUPPORTED;
		}
		sc_asn1_put_tag(0x82, buf, 2, p, *outlen - (p - out), &p);
	}
	else if( file->type == SC_FILE_TYPE_BSO ) {
		buf[0] = 0x10;
		buf[1] = 0x00;
		sc_asn1_put_tag(0x82, buf, 2, p, *outlen - (p - out), &p);
	}

	buf[0] = (file->id >> 8) & 0xFF;
	buf[1] = file->id & 0xFF;
	sc_asn1_put_tag(0x83, buf, 2, p, *outlen - (p - out), &p);
	if (file->type == SC_FILE_TYPE_DF) {
		if (file->namelen != 0) {
		sc_asn1_put_tag(0x84, file->name, file->namelen, p, *outlen - (p - out), &p);
		}
		else {
			return SC_ERROR_INVALID_ARGUMENTS;
		}
	}
	if (file->type == SC_FILE_TYPE_DF) {
		/* 127 files at most */
		sc_asn1_put_tag(0x85, "\x00\x7f", 2, p, *outlen - (p - out), &p);
	}
	else if (file->type == SC_FILE_TYPE_BSO) {
		buf[0] = file->size & 0xff;
		sc_asn1_put_tag(0x85, buf, 1, p, *outlen - (p - out), &p);
	}
	else if (file->type == SC_FILE_TYPE_INTERNAL_EF ){
		if (file->ef_structure == SC_CARDCTL_OBERTHUR_KEY_RSA_CRT ||
				file->ef_structure == SC_CARDCTL_OBERTHUR_KEY_RSA_PUBLIC )
		{
			buf[0] = (file->size >> 8) & 0xFF;
			buf[1] = file->size & 0xFF;
		sc_asn1_put_tag(0x85, buf, 2, p, *outlen - (p - out), &p);
		}
	}
	if (file->sec_attr_len) {
		memcpy(buf, file->sec_attr, file->sec_attr_len);
		sc_asn1_put_tag(0x86, buf, file->sec_attr_len,
				p, *outlen - (p - out), &p);
	}
	else
	{
		sc_debug(card->ctx, SC_LOG_DEBUG_NORMAL, "SC_FILE_ACL\n");
		if (file->type == SC_FILE_TYPE_DF) {
			ops[0] = SC_AC_OP_LIST_FILES;
			ops[1] = SC_AC_OP_CREATE;
			ops[3] = SC_AC_OP_DELETE;
		}
		else if (file->type == SC_FILE_TYPE_WORKING_EF) {
			if (file->ef_structure == SC_FILE_EF_TRANSPARENT) {
				ops[0] = SC_AC_OP_READ;
				ops[1] = SC_AC_OP_UPDATE;
				ops[3] = SC_AC_OP_DELETE;
			}
			else if (file->ef_structure == SC_FILE_EF_LINEAR_FIXED ||
					file->ef_structure == SC_FILE_EF_LINEAR_VARIABLE) {
				ops[0] = SC_AC_OP_READ;
				ops[1] = SC_AC_OP_UPDATE;
				ops[2] = SC_AC_OP_WRITE;
				ops[3] = SC_AC_OP_DELETE;
			}
			else {
				return SC_ERROR_NOT_SUPPORTED;
			}
		}
		else if (file->type == SC_FILE_TYPE_BSO) {
			ops[0] = SC_AC_OP_UPDATE;
			ops[3] = SC_AC_OP_DELETE;
		}
		else if (file->type == SC_FILE_TYPE_INTERNAL_EF) {
			if (file->ef_structure == SC_CARDCTL_OBERTHUR_KEY_RSA_CRT) {
				ops[1] = SC_AC_OP_UPDATE;
				ops[2] = SC_AC_OP_CRYPTO;
				ops[3] = SC_AC_OP_DELETE;
			}
			else if (file->ef_structure == SC_CARDCTL_OBERTHUR_KEY_RSA_PUBLIC) {
				ops[0] = SC_AC_OP_READ;
				ops[1] = SC_AC_OP_UPDATE;
				ops[2] = SC_AC_OP_CRYPTO;
				ops[3] = SC_AC_OP_DELETE;
			}
		}
		else
		{
			return SC_ERROR_NOT_SUPPORTED;
		}
		for (ii = 0; ii < sizeof(ops); ii++) {
			const struct sc_acl_entry *entry;

			buf[ii] = 0xFF;
			if (ops[ii] == 0xFF)
				continue;
			entry = sc_file_get_acl_entry(file, ops[ii]);
			rv = acl_to_ac_byte(card, entry);
			SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, rv, "Invalid ACL");
			buf[ii] = rv;
		}
		sc_asn1_put_tag(0x86, buf, sizeof(ops),
				p, *outlen - (p - out), &p);

	}

	if (file->ef_structure == SC_CARDCTL_OBERTHUR_KEY_RSA_PUBLIC) {
		sc_asn1_put_tag(0x87, "\x00\x66", 2,
				p, *outlen - (p - out), &p);
	}

	out[1] = p - out - 2;

	*outlen = p - out;
	return 0;
}

static int epass2003_create_file(struct sc_card *card, sc_file_t * file)
{
	int r;
	size_t len;
	u8 sbuf[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	struct sc_apdu apdu;

	len = SC_MAX_APDU_BUFFER_SIZE;

	epass2003_hook_file(file, 1);

	if (card->ops->construct_fci == NULL)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_NOT_SUPPORTED);
	r = epass2003_construct_fci(card, file, sbuf, &len);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "construct_fci() failed");

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE0, 0x00, 0x00);
	apdu.lc = len;
	apdu.datalen = len;
	apdu.data = sbuf;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU sw1/2 wrong");
	epass2003_hook_file(file, 0);
	return r;
}

static int epass2003_delete_file(struct sc_card *card, const sc_path_t * path)
{
	int r;
	u8 sbuf[2];
	struct sc_apdu apdu;

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	r = sc_select_file(card, path, NULL);
	epass2003_hook_path((struct sc_path *)path, 1);
	if (r == SC_SUCCESS) {
		sbuf[0] = path->value[path->len - 2];
		sbuf[1] = path->value[path->len - 1];
		sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xE4, 0x00, 0x00);
		apdu.lc = 2;
		apdu.datalen = 2;
		apdu.data = sbuf;
	}
	else 
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INVALID_ARGUMENTS);

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	return sc_check_sw(card, apdu.sw1, apdu.sw2);
}
static int
epass2003_list_files(struct sc_card *card, unsigned char *buf, size_t buflen)
{
	struct sc_apdu apdu;
	unsigned char rbuf[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	int rv;

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0x34, 0x00, 0x00);
	apdu.cla = 0x80;
	apdu.le = 0x40;
	apdu.resplen = sizeof(rbuf);
	apdu.resp = rbuf;

	rv = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, rv, "APDU transmit failed");

	rv = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, rv, "Card returned error");

	if (apdu.resplen == 0x100 && rbuf[0] == 0 && rbuf[1] == 0)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, 0);

	buflen = buflen < apdu.resplen ? buflen : apdu.resplen;
	memcpy(buf, rbuf, buflen);

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, buflen);
}

static int internal_write_rsa_key_factor(struct sc_card *card,
					 unsigned short fid, u8 factor,
					 sc_pkcs15_bignum_t data)
{
	int r;
	struct sc_apdu apdu;
	u8 sbuff[SC_MAX_EXT_APDU_BUFFER_SIZE] = { 0 };

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	sbuff[0] = ((fid & 0xff00) >> 8);
	sbuff[1] = (fid & 0x00ff);
	memcpy(&sbuff[2], data.data, data.len);
	sc_mem_reverse(&sbuff[2], data.len);

	sc_format_apdu(card, &apdu, SC_APDU_CASE_3, 0xe7, factor, 0x00);
	apdu.cla = 0x80;
	apdu.lc = apdu.datalen = 2 + data.len;
	apdu.data = sbuff;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r,"Write prkey factor failed");
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int internal_write_rsa_key(struct sc_card *card, unsigned short fid, struct sc_pkcs15_prkey_rsa *rsa)
{
	int r;

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	r = internal_write_rsa_key_factor(card, fid, 0x02, rsa->modulus);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "write n failed");
	r = internal_write_rsa_key_factor(card, fid, 0x03, rsa->d);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "write d failed");

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int hash_data(unsigned char *data, size_t datalen, unsigned char *hash)
{
	unsigned char data_hash[24] = { 0 };
	size_t len = 0;
	if((NULL == data) || (NULL == hash))
	{
		return SC_ERROR_INVALID_ARGUMENTS;
	}

	sha1_digest(data, datalen, data_hash);

	len = REVERSE_ORDER4(datalen);
	memcpy(&data_hash[20], &len, 4);
	memcpy(hash, data_hash, 24);
	return SC_SUCCESS;
}

static int install_secret_key(struct sc_card *card, unsigned char ktype, unsigned char kid, unsigned char useac, unsigned char modifyac, unsigned char EC, unsigned char* data, unsigned long dataLen)
{
	int r;
	struct sc_apdu apdu;
	unsigned char isapp = 0x00;	/* appendable */
	unsigned char tmp_data[256] = { 0 };
	tmp_data[0] = ktype;
	tmp_data[1] = kid;
	tmp_data[2] = useac;
	tmp_data[3] = modifyac;
	tmp_data[8] = 0xFF;
	if (0x04==ktype || 0x06==ktype)
	{
		tmp_data[4] = EPASS2003_AC_MAC_NOLESS | EPASS2003_AC_SO;
		tmp_data[5] = EPASS2003_AC_MAC_NOLESS | EPASS2003_AC_SO;
		tmp_data[7] = (kid==PIN_ID[0]?EPASS2003_AC_USER:EPASS2003_AC_SO);
		tmp_data[9] = (EC<<4)|EC;
	}
	memcpy(&tmp_data[10], data, dataLen);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xe3, isapp, 0x00);
	apdu.cla = 0x80;
	apdu.lc = apdu.datalen = 10 + dataLen;
	apdu.data = tmp_data;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU install_secret_key failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "install_secret_key failed");
	return r;
}

static int internal_install_pre(struct sc_card *card)
{
	int r, i, j;
	unsigned char data[32] = { 0 };
	/* init key for enc */
	r = install_secret_key(card, 0x01, 0x00, EPASS2003_AC_MAC_NOLESS|EPASS2003_AC_EVERYONE, EPASS2003_AC_MAC_NOLESS|EPASS2003_AC_EVERYONE, 0, g_init_key_enc, 16);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "Install init key failed");
	/* init key for mac */
	r = install_secret_key(card, 0x02, 0x00, EPASS2003_AC_MAC_NOLESS|EPASS2003_AC_EVERYONE, EPASS2003_AC_MAC_NOLESS|EPASS2003_AC_EVERYONE, 0, g_init_key_mac, 16);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "Install init key failed");
	return r;
}

static int internal_install_pin(struct sc_card *card, sc_epass2003_wkey_data *pin)
	/* use external auth secret as pin */
{
	int r, i, j;
	unsigned char data[32] = { 0 };
	unsigned char hash[HASH_LEN] = { 0 };
	r = hash_data(pin->key_data.es_secret.key_val, pin->key_data.es_secret.key_len, hash);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "hash data failed");
	r = install_secret_key(card, 0x04, pin->key_data.es_secret.kid,
			pin->key_data.es_secret.ac[0], pin->key_data.es_secret.ac[1], 
			pin->key_data.es_secret.EC, hash, HASH_LEN);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "Install failed");
	return r;
}

static int epass2003_write_key(struct sc_card *card, sc_epass2003_wkey_data *data)
{
	SC_FUNC_CALLED(card->ctx, 1);

	if (data->type & SC_EPASS2003_KEY)
	{
		if( data->type == SC_EPASS2003_KEY_RSA )
		{
			return internal_write_rsa_key(card, data->key_data.es_key.fid, 
					data->key_data.es_key.rsa);
		}
		else
		{
			SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_NOT_SUPPORTED);
		}
	}
	else if(data->type & SC_EPASS2003_SECRET)
	{
		if( data->type == SC_EPASS2003_SECRET_PRE )
		{
			return internal_install_pre(card);
		}
		else if( data->type == SC_EPASS2003_SECRET_PIN)
		{
			return internal_install_pin(card, data);
		}
		else
		{
			SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_NOT_SUPPORTED);
		}
	}
	else
	{
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_NOT_SUPPORTED);
	}
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int epass2003_gen_key(struct sc_card *card, sc_epass2003_gen_key_data *data)
{
	int r;
	size_t len = data->key_length;
	struct sc_apdu apdu;
	u8 rbuf[SC_MAX_EXT_APDU_BUFFER_SIZE] = { 0 };
	u8 sbuf[SC_MAX_EXT_APDU_BUFFER_SIZE] = { 0 }, *p;
	struct sc_path tmp_path;

	SC_FUNC_CALLED(card->ctx, 1);

	sbuf[0] = 0x01;
	sbuf[1] = (u8) ((len >> 8) & 0xff);
	sbuf[2] = (u8) (len & 0xff);
	sbuf[3] = (u8) ((data->prkey_id >> 8) & 0xFF);
	sbuf[4] = (u8) ((data->prkey_id) & 0xFF);
	sbuf[5] = (u8) ((data->pukey_id >> 8) & 0xFF);
	sbuf[6] = (u8) ((data->pukey_id) & 0xFF);

	/* generate key */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x46, 0x00, 0x00);
	apdu.lc = apdu.datalen = 7;
	apdu.data = sbuf;

	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r,"generate keypair failed");

	/* read public key */
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xb4, 0x02, 0x00);
	apdu.cla = 0x80;
	apdu.lc = apdu.datalen = 2;
	apdu.data = &sbuf[5];
	apdu.resp = rbuf;
	apdu.resplen = sizeof(rbuf);
	apdu.le = 0x00;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU transmit failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "get pukey failed");

	if( len < apdu.resplen )
	{
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_INVALID_ARGUMENTS);
	}
	data->modulus = (u8 *) malloc(len);
	if (!data->modulus)
		SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_ERROR_OUT_OF_MEMORY);

	memcpy(data->modulus, rbuf, len);

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int epass2003_erase_card(struct sc_card *card)
{
	int r;
	u8 sbuf[2];
	struct sc_apdu apdu;

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);
	card->cache.valid = 0;
	r = sc_delete_file(card, sc_get_mf_path());

	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "delete MF failed");
	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, r);
}

static int epass2003_get_serialnr(struct sc_card *card, sc_serial_number_t *serial)
{
	int r;
	struct sc_apdu apdu;
	u8 rbuf[8];
	size_t rbuf_len = sizeof(rbuf);

	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	if (SC_SUCCESS != get_data(card, 0x80, rbuf, rbuf_len))
	{
		return SC_ERROR_CARD_CMD_FAILED;
	}

	card->serialnr.len = serial->len = 8;
	memcpy(card->serialnr.value, rbuf, 8);
	memcpy(serial->value, rbuf, 8);

	SC_FUNC_RETURN(card->ctx, SC_LOG_DEBUG_VERBOSE, SC_SUCCESS);
}

static int epass2003_card_ctl(struct sc_card *card, unsigned long cmd, void *ptr)
{
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);

	switch (cmd)
	{
	case SC_CARDCTL_ENTERSAFE_WRITE_KEY:
			return epass2003_write_key(card, (sc_epass2003_wkey_data *)ptr);
	case SC_CARDCTL_ENTERSAFE_GENERATE_KEY:
			return epass2003_gen_key(card, (sc_epass2003_gen_key_data *)ptr);
	case SC_CARDCTL_ERASE_CARD:
		return epass2003_erase_card(card);
	case SC_CARDCTL_GET_SERIALNR:
		return epass2003_get_serialnr(card, (sc_serial_number_t *) ptr);
	default:
		return SC_ERROR_NOT_SUPPORTED;
	}
}

static void internal_sanitize_pin_info(struct sc_pin_cmd_pin *pin, unsigned int num)
{
	pin->encoding = SC_PIN_ENCODING_ASCII;
	pin->min_length = 4;
	pin->max_length = 16;
	pin->pad_length = 16;
	pin->offset = 5 + num * 16;
	pin->pad_char = 0x00;
}

static int get_external_key_retries(struct sc_card *card, unsigned char kid, unsigned char* retries)
{
	int r;
	struct sc_apdu apdu;
	unsigned char random[16] = { 0 };
	unsigned char resp[SC_MAX_APDU_BUFFER_SIZE] = { 0 };
	size_t resplen = SC_MAX_APDU_BUFFER_SIZE;
	r = sc_get_challenge(card, random, 8);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "get challenge get_external_key_retries failed");

	sc_format_apdu(card, &apdu, SC_APDU_CASE_2_SHORT, 0x82, 0x01, 0x80|kid); 
	apdu.resp = resp;
	apdu.resplen = resplen;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU get_external_key_retries failed");
	if (retries && ((0x63 == (apdu.sw1 & 0xff)) && (0xC0 == (apdu.sw2 & 0xf0))))
	{
		*retries = (apdu.sw2 & 0x0f);
		r = SC_SUCCESS;
	}
	else
	{
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "get_external_key_retries failed");
	}
	return r;
}

static int external_key_auth(struct sc_card *card, unsigned char kid, unsigned char* data, size_t datalen)
{
	int r;
	struct sc_apdu apdu;
	unsigned char random[16] = { 0 };
	unsigned char tmp_data[16] = { 0 };
	unsigned char hash[HASH_LEN] = { 0 };
	unsigned char iv[16] = { 0 };
	r = sc_get_challenge(card, random, 8);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "get challenge external_key_auth failed");
	r = hash_data(data, datalen, hash);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "hash data failed");
	des3_encrypt_cbc(hash, HASH_LEN, iv, random, 8, tmp_data);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0x82, 0x01, 0x80|kid);
	apdu.lc = apdu.datalen = 8;
	apdu.data = tmp_data;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU external_key_auth failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "external_key_auth failed");
	return r;
}

static int update_secret_key(struct sc_card *card, unsigned char ktype, unsigned char kid, unsigned char* data, unsigned long datalen)
{
	int r;
	struct sc_apdu apdu;
	unsigned char hash[HASH_LEN] = { 0 };
	unsigned char tmp_data[256] = { 0 };
	r = hash_data(data, datalen, hash);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "hash data failed");
	tmp_data[0] = (MAX_PIN_COUNTER << 4) | MAX_PIN_COUNTER;
	memcpy(&tmp_data[1], hash, HASH_LEN);
	sc_format_apdu(card, &apdu, SC_APDU_CASE_3_SHORT, 0xe5, ktype, kid);
	apdu.cla = 0x80;
	apdu.lc = apdu.datalen = 1 + HASH_LEN;
	apdu.data = tmp_data;
	r = sc_transmit_apdu(card, &apdu);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "APDU update_secret_key failed");
	r = sc_check_sw(card, apdu.sw1, apdu.sw2);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "update_secret_key failed");
	return r;
}

/* use external auth secret as pin */
static int epass2003_pin_cmd(struct sc_card *card, struct sc_pin_cmd_data *data,
			     int *tries_left)
{
	int r;
	u8 kid;
	SC_FUNC_CALLED(card->ctx, SC_LOG_DEBUG_VERBOSE);
	internal_sanitize_pin_info(&data->pin1, 0);
	internal_sanitize_pin_info(&data->pin2, 1);
	data->flags |= SC_PIN_CMD_NEED_PADDING;
	kid = data->pin_reference;
	/* get pin retries */
	if( data->cmd == SC_PIN_CMD_GET_INFO )
	{
		u8 retries = 0;
		r = get_external_key_retries(card, 0x80 | kid, &retries);
		if( r == SC_SUCCESS )
		{
			data->pin1.max_tries = MAX_PIN_COUNTER;
			data->pin1.tries_left = retries;
		}
		return r;
	}
	/* verify */
	if (data->cmd == SC_PIN_CMD_UNBLOCK) {
	r = external_key_auth(card, (kid+1), (unsigned char *)data->pin1.data, data->pin1.len);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "verify pin failed");
	}
	else {
	r = external_key_auth(card, kid, (unsigned char *)data->pin1.data, data->pin1.len);
	SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "verify pin failed");

	}

	if( data->cmd == SC_PIN_CMD_CHANGE ||
			data->cmd == SC_PIN_CMD_UNBLOCK )
	{
		/* change */
		r = update_secret_key(card, 0x04, kid, (unsigned char *)data->pin2.data, data->pin2.len);
		SC_TEST_RET(card->ctx, SC_LOG_DEBUG_NORMAL, r, "verify pin failed");
	}
	return r;
}

static struct sc_card_driver *sc_get_driver(void)
{
	struct sc_card_driver *iso_drv = sc_get_iso7816_driver();

	if (iso_ops == NULL)
		iso_ops = iso_drv->ops;

	epass2003_ops = *iso_ops;

	epass2003_ops.match_card = epass2003_match_card;
	epass2003_ops.init = epass2003_init;
	epass2003_ops.sm_wrap_apdu = epass2003_sm_wrap_apdu;
	epass2003_ops.sm_unwrap_apdu = epass2003_sm_unwrap_apdu;
	epass2003_ops.write_binary = NULL;
	epass2003_ops.write_record = NULL;
	epass2003_ops.select_file = epass2003_select_file;
	epass2003_ops.get_response = NULL;
	epass2003_ops.restore_security_env = epass2003_restore_security_env;
	epass2003_ops.set_security_env = epass2003_set_security_env;
	epass2003_ops.decipher = epass2003_decipher;
	epass2003_ops.compute_signature = epass2003_decipher;
	epass2003_ops.create_file = epass2003_create_file;
	epass2003_ops.delete_file = epass2003_delete_file;
	/* epass2003_ops.list_files = epass2003_list_files; */
	epass2003_ops.card_ctl = epass2003_card_ctl;
	epass2003_ops.process_fci = epass2003_process_fci;
	epass2003_ops.construct_fci = epass2003_construct_fci;
	epass2003_ops.pin_cmd = epass2003_pin_cmd;
	return &epass2003_drv;
}

struct sc_card_driver *sc_get_epass2003_driver(void)
{
	return sc_get_driver();
}
#endif
