/*
 * Utility functions for SSL:
 * Mostly generic functions that retrieve information from certificates
 *
 * Copyright (C) 2012 EXCELIANCE, Emeric Brun <ebrun@exceliance.fr>
 * Copyright (C) 2020 HAProxy Technologies, William Lallemand <wlallemand@haproxy.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */


#include <haproxy/api.h>
#include <haproxy/buf-t.h>
#include <haproxy/chunk.h>
#include <haproxy/openssl-compat.h>
#include <haproxy/ssl_sock.h>
#include <haproxy/ssl_utils.h>

/* fill a buffer with the algorithm and size of a public key */
int cert_get_pkey_algo(X509 *crt, struct buffer *out)
{
	int bits = 0;
	int sig = TLSEXT_signature_anonymous;
	int len = -1;
	EVP_PKEY *pkey;

	pkey = X509_get_pubkey(crt);
	if (pkey) {
		bits = EVP_PKEY_bits(pkey);
		switch(EVP_PKEY_base_id(pkey)) {
		case EVP_PKEY_RSA:
			sig = TLSEXT_signature_rsa;
			break;
		case EVP_PKEY_EC:
			sig = TLSEXT_signature_ecdsa;
			break;
		case EVP_PKEY_DSA:
			sig = TLSEXT_signature_dsa;
			break;
		}
		EVP_PKEY_free(pkey);
	}

	switch(sig) {
	case TLSEXT_signature_rsa:
		len = chunk_printf(out, "RSA%d", bits);
		break;
	case TLSEXT_signature_ecdsa:
		len = chunk_printf(out, "EC%d", bits);
		break;
	case TLSEXT_signature_dsa:
		len = chunk_printf(out, "DSA%d", bits);
		break;
	default:
		return 0;
	}
	if (len < 0)
		return 0;
	return 1;
}

/* Extract a serial from a cert, and copy it to a chunk.
 * Returns 1 if serial is found and copied, 0 if no serial found and
 * -1 if output is not large enough.
 */
int ssl_sock_get_serial(X509 *crt, struct buffer *out)
{
	ASN1_INTEGER *serial;

	serial = X509_get_serialNumber(crt);
	if (!serial)
		return 0;

	if (out->size < serial->length)
		return -1;

	memcpy(out->area, serial->data, serial->length);
	out->data = serial->length;
	return 1;
}

/* Extract a cert to der, and copy it to a chunk.
 * Returns 1 if the cert is found and copied, 0 on der conversion failure
 * and -1 if the output is not large enough.
 */
int ssl_sock_crt2der(X509 *crt, struct buffer *out)
{
	int len;
	unsigned char *p = (unsigned char *) out->area;

	len = i2d_X509(crt, NULL);
	if (len <= 0)
		return 1;

	if (out->size < len)
		return -1;

	i2d_X509(crt, &p);
	out->data = len;
	return 1;
}


/* Copy Date in ASN1_UTCTIME format in struct buffer out.
 * Returns 1 if serial is found and copied, 0 if no valid time found
 * and -1 if output is not large enough.
 */
int ssl_sock_get_time(ASN1_TIME *tm, struct buffer *out)
{
	if (tm->type == V_ASN1_GENERALIZEDTIME) {
		ASN1_GENERALIZEDTIME *gentm = (ASN1_GENERALIZEDTIME *)tm;

		if (gentm->length < 12)
			return 0;
		if (gentm->data[0] != 0x32 || gentm->data[1] != 0x30)
			return 0;
		if (out->size < gentm->length-2)
			return -1;

		memcpy(out->area, gentm->data+2, gentm->length-2);
		out->data = gentm->length-2;
		return 1;
	}
	else if (tm->type == V_ASN1_UTCTIME) {
		ASN1_UTCTIME *utctm = (ASN1_UTCTIME *)tm;

		if (utctm->length < 10)
			return 0;
		if (utctm->data[0] >= 0x35)
			return 0;
		if (out->size < utctm->length)
			return -1;

		memcpy(out->area, utctm->data, utctm->length);
		out->data = utctm->length;
		return 1;
	}

	return 0;
}

/* Extract an entry from a X509_NAME and copy its value to an output chunk.
 * Returns 1 if entry found, 0 if entry not found, or -1 if output not large enough.
 */
int ssl_sock_get_dn_entry(X509_NAME *a, const struct buffer *entry, int pos,
                          struct buffer *out)
{
	X509_NAME_ENTRY *ne;
	ASN1_OBJECT *obj;
	ASN1_STRING *data;
	const unsigned char *data_ptr;
	int data_len;
	int i, j, n;
	int cur = 0;
	const char *s;
	char tmp[128];
	int name_count;

	name_count = X509_NAME_entry_count(a);

	out->data = 0;
	for (i = 0; i < name_count; i++) {
		if (pos < 0)
			j = (name_count-1) - i;
		else
			j = i;

		ne = X509_NAME_get_entry(a, j);
		obj = X509_NAME_ENTRY_get_object(ne);
		data = X509_NAME_ENTRY_get_data(ne);
		data_ptr = ASN1_STRING_get0_data(data);
		data_len = ASN1_STRING_length(data);
		n = OBJ_obj2nid(obj);
		if ((n == NID_undef) || ((s = OBJ_nid2sn(n)) == NULL)) {
			i2t_ASN1_OBJECT(tmp, sizeof(tmp), obj);
			s = tmp;
		}

		if (chunk_strcasecmp(entry, s) != 0)
			continue;

		if (pos < 0)
			cur--;
		else
			cur++;

		if (cur != pos)
			continue;

		if (data_len > out->size)
			return -1;

		memcpy(out->area, data_ptr, data_len);
		out->data = data_len;
		return 1;
	}

	return 0;

}

/*
 * Extract the DN in the specified format from the X509_NAME and copy result to a chunk.
 * Currently supports rfc2253 for returning LDAP V3 DNs.
 * Returns 1 if dn entries exist, 0 if no dn entry was found.
 */
int ssl_sock_get_dn_formatted(X509_NAME *a, const struct buffer *format, struct buffer *out)
{
	BIO *bio = NULL;
	int ret = 0;
	int data_len = 0;

	if (chunk_strcmp(format, "rfc2253") == 0) {
		bio = BIO_new(BIO_s_mem());
		if (bio == NULL)
			goto out;

		if (X509_NAME_print_ex(bio, a, 0, XN_FLAG_RFC2253) < 0)
			goto out;

		if ((data_len = BIO_read(bio, out->area, out->size)) <= 0)
			goto out;

		out->data = data_len;

		ret = 1;
	}
out:
	if (bio)
		BIO_free(bio);
	return ret;
}

/* Extract and format full DN from a X509_NAME and copy result into a chunk
 * Returns 1 if dn entries exits, 0 if no dn entry found or -1 if output is not large enough.
 */
int ssl_sock_get_dn_oneline(X509_NAME *a, struct buffer *out)
{
	X509_NAME_ENTRY *ne;
	ASN1_OBJECT *obj;
	ASN1_STRING *data;
	const unsigned char *data_ptr;
	int data_len;
	int i, n, ln;
	int l = 0;
	const char *s;
	char *p;
	char tmp[128];
	int name_count;


	name_count = X509_NAME_entry_count(a);

	out->data = 0;
	p = out->area;
	for (i = 0; i < name_count; i++) {
		ne = X509_NAME_get_entry(a, i);
		obj = X509_NAME_ENTRY_get_object(ne);
		data = X509_NAME_ENTRY_get_data(ne);
		data_ptr = ASN1_STRING_get0_data(data);
		data_len = ASN1_STRING_length(data);
		n = OBJ_obj2nid(obj);
		if ((n == NID_undef) || ((s = OBJ_nid2sn(n)) == NULL)) {
			i2t_ASN1_OBJECT(tmp, sizeof(tmp), obj);
			s = tmp;
		}
		ln = strlen(s);

		l += 1 + ln + 1 + data_len;
		if (l > out->size)
			return -1;
		out->data = l;

		*(p++)='/';
		memcpy(p, s, ln);
		p += ln;
		*(p++)='=';
		memcpy(p, data_ptr, data_len);
		p += data_len;
	}

	if (!out->data)
		return 0;

	return 1;
}


extern int ssl_client_crt_ref_index;

/*
 * This function fetches the SSL certificate for a specific connection (either
 * client certificate or server certificate depending on the cert_peer
 * parameter).
 * When trying to get the peer certificate from the server side, we first try to
 * use the dedicated SSL_get_peer_certificate function, but we fall back to
 * trying to get the client certificate reference that might have been stored in
 * the SSL structure's ex_data during the verification process.
 * Returns NULL in case of failure.
 */
X509* ssl_sock_get_peer_certificate(SSL *ssl)
{
	X509* cert;

	cert = SSL_get_peer_certificate(ssl);
	/* Get the client certificate reference stored in the SSL
	 * structure's ex_data during the verification process. */
	if (!cert) {
		cert = SSL_get_ex_data(ssl, ssl_client_crt_ref_index);
		if (cert)
			X509_up_ref(cert);
	}

	return cert;
}

/*
 * This function fetches the x509* for the root CA of client certificate
 * from the verified chain. We use the SSL_get0_verified_chain and get the
 * last certificate in the x509 stack.
 *
 * Returns NULL in case of failure.
*/
#ifdef HAVE_SSL_get0_verified_chain
X509* ssl_sock_get_verified_chain_root(SSL *ssl)
{
	STACK_OF(X509) *chain = NULL;
	X509 *crt = NULL;
	int i;

	chain = SSL_get0_verified_chain(ssl);
	if (!chain)
		return NULL;

	for (i = 0; i < sk_X509_num(chain); i++) {
		crt = sk_X509_value(chain, i);

		if (X509_check_issued(crt, crt) == X509_V_OK)
			break;
	}

	return crt;
}
#endif

/*
 * Take an OpenSSL version in text format and return a numeric openssl version
 * Return 0 if it failed to parse the version
 *
 *  https://www.openssl.org/docs/man1.1.1/man3/OPENSSL_VERSION_NUMBER.html
 *
 *  MNNFFPPS: major minor fix patch status
 *
 *  The status nibble has one of the values 0 for development, 1 to e for betas
 *  1 to 14, and f for release.
 *
 *  for example
 *
 *   0x0090821f     0.9.8zh
 *   0x1000215f     1.0.2u
 *   0x30000000     3.0.0-alpha17
 *   0x30000002     3.0.0-beta2
 *   0x3000000e     3.0.0-beta14
 *   0x3000000f     3.0.0
 */
unsigned int openssl_version_parser(const char *version)
{
	unsigned int numversion;
	unsigned int major = 0, minor = 0, fix = 0, patch = 0, status = 0;
	char *p, *end;

	p = (char *)version;

	if (!p || !*p)
		return 0;

	major = strtol(p, &end, 10);
	if (*end != '.' || major > 0xf)
		goto error;
	p = end + 1;

	minor = strtol(p, &end, 10);
	if (*end != '.' || minor > 0xff)
		goto error;
	p = end + 1;

	fix = strtol(p, &end, 10);
	if (fix > 0xff)
		goto error;
	p = end;

	if (!*p) {
		/* end of the string, that's a release */
		status = 0xf;
	} else if (*p == '-') {
		/* after the hyphen, only the beta will increment the status
		 * counter, all others versions will be considered as "dev" and
		 * does not increment anything */
		p++;

		if (!strncmp(p, "beta", 4)) {
			p += 4;
			status = strtol(p, &end, 10);
			if (status > 14)
				goto error;
		}
	} else {
		/* that's a patch release */
		patch = 1;

		/* add the value of each letter */
		while (*p) {
			patch += (*p & ~0x20) - 'A';
			p++;
		}
		status = 0xf;
	}

end:
	numversion =  ((major & 0xf) << 28) | ((minor & 0xff) << 20) | ((fix & 0xff) << 12) | ((patch & 0xff) << 4) | (status & 0xf);
	return numversion;

error:
	return 0;

}

/* Exclude GREASE (RFC8701) values from input buffer */
void exclude_tls_grease(char *input, int len, struct buffer *output)
{
	int ptr = 0;

	while (ptr < len - 1) {
		if (input[ptr] != input[ptr+1] || (input[ptr] & 0x0f) != 0x0a) {
			if (output->data <= output->size - 2) {
				memcpy(output->area + output->data, input + ptr, 2);
				output->data += 2;
			} else
				break;
		}
		ptr += 2;
	}
	if (output->size - output->data > 0 && len - ptr > 0)
		output->area[output->data++] = input[ptr];
}

/*
 * The following generates an array <x509_v_codes> in which the X509_V_ERR_*
 * codes are populated with there string equivalent. Depending on the version
 * of the SSL library, some code does not exist, these will be populated as
 * "-1" in the array.
 *
 * The list was taken from
 * https://github.com/openssl/openssl/blob/master/include/openssl/x509_vfy.h.in
 * and must be updated when new constant are introduced.
 */

#undef _Q
#define _Q(x) (#x)
#undef V
#define V(x) { .code = -1, .value = _Q(x), .string = #x }

static struct x509_v_codes {
	int code;             // integer value of the code or -1 if undefined
	const char *value;    // value of the macro as a string or its name
	const char *string;   // name of the macro
} x509_v_codes[] = {
	V(X509_V_OK),
	V(X509_V_ERR_UNSPECIFIED),
	V(X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT),
	V(X509_V_ERR_UNABLE_TO_GET_CRL),
	V(X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE),
	V(X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE),
	V(X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY),
	V(X509_V_ERR_CERT_SIGNATURE_FAILURE),
	V(X509_V_ERR_CRL_SIGNATURE_FAILURE),
	V(X509_V_ERR_CERT_NOT_YET_VALID),
	V(X509_V_ERR_CERT_HAS_EXPIRED),
	V(X509_V_ERR_CRL_NOT_YET_VALID),
	V(X509_V_ERR_CRL_HAS_EXPIRED),
	V(X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD),
	V(X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD),
	V(X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD),
	V(X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD),
	V(X509_V_ERR_OUT_OF_MEM),
	V(X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT),
	V(X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN),
	V(X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY),
	V(X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE),
	V(X509_V_ERR_CERT_CHAIN_TOO_LONG),
	V(X509_V_ERR_CERT_REVOKED),
	V(X509_V_ERR_NO_ISSUER_PUBLIC_KEY),
	V(X509_V_ERR_PATH_LENGTH_EXCEEDED),
	V(X509_V_ERR_INVALID_PURPOSE),
	V(X509_V_ERR_CERT_UNTRUSTED),
	V(X509_V_ERR_CERT_REJECTED),
	V(X509_V_ERR_SUBJECT_ISSUER_MISMATCH),
	V(X509_V_ERR_AKID_SKID_MISMATCH),
	V(X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH),
	V(X509_V_ERR_KEYUSAGE_NO_CERTSIGN),
	V(X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER),
	V(X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION),
	V(X509_V_ERR_KEYUSAGE_NO_CRL_SIGN),
	V(X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION),
	V(X509_V_ERR_INVALID_NON_CA),
	V(X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED),
	V(X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE),
	V(X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED),
	V(X509_V_ERR_INVALID_EXTENSION),
	V(X509_V_ERR_INVALID_POLICY_EXTENSION),
	V(X509_V_ERR_NO_EXPLICIT_POLICY),
	V(X509_V_ERR_DIFFERENT_CRL_SCOPE),
	V(X509_V_ERR_UNSUPPORTED_EXTENSION_FEATURE),
	V(X509_V_ERR_UNNESTED_RESOURCE),
	V(X509_V_ERR_PERMITTED_VIOLATION),
	V(X509_V_ERR_EXCLUDED_VIOLATION),
	V(X509_V_ERR_SUBTREE_MINMAX),
	V(X509_V_ERR_APPLICATION_VERIFICATION),
	V(X509_V_ERR_UNSUPPORTED_CONSTRAINT_TYPE),
	V(X509_V_ERR_UNSUPPORTED_CONSTRAINT_SYNTAX),
	V(X509_V_ERR_UNSUPPORTED_NAME_SYNTAX),
	V(X509_V_ERR_CRL_PATH_VALIDATION_ERROR),
	V(X509_V_ERR_PATH_LOOP),
	V(X509_V_ERR_SUITE_B_INVALID_VERSION),
	V(X509_V_ERR_SUITE_B_INVALID_ALGORITHM),
	V(X509_V_ERR_SUITE_B_INVALID_CURVE),
	V(X509_V_ERR_SUITE_B_INVALID_SIGNATURE_ALGORITHM),
	V(X509_V_ERR_SUITE_B_LOS_NOT_ALLOWED),
	V(X509_V_ERR_SUITE_B_CANNOT_SIGN_P_384_WITH_P_256),
	V(X509_V_ERR_HOSTNAME_MISMATCH),
	V(X509_V_ERR_EMAIL_MISMATCH),
	V(X509_V_ERR_IP_ADDRESS_MISMATCH),
	V(X509_V_ERR_DANE_NO_MATCH),
	V(X509_V_ERR_EE_KEY_TOO_SMALL),
	V(X509_V_ERR_CA_KEY_TOO_SMALL),
	V(X509_V_ERR_CA_MD_TOO_WEAK),
	V(X509_V_ERR_INVALID_CALL),
	V(X509_V_ERR_STORE_LOOKUP),
	V(X509_V_ERR_NO_VALID_SCTS),
	V(X509_V_ERR_PROXY_SUBJECT_NAME_VIOLATION),
	V(X509_V_ERR_OCSP_VERIFY_NEEDED),
	V(X509_V_ERR_OCSP_VERIFY_FAILED),
	V(X509_V_ERR_OCSP_CERT_UNKNOWN),
	V(X509_V_ERR_UNSUPPORTED_SIGNATURE_ALGORITHM),
	V(X509_V_ERR_SIGNATURE_ALGORITHM_MISMATCH),
	V(X509_V_ERR_SIGNATURE_ALGORITHM_INCONSISTENCY),
	V(X509_V_ERR_INVALID_CA),
	V(X509_V_ERR_PATHLEN_INVALID_FOR_NON_CA),
	V(X509_V_ERR_PATHLEN_WITHOUT_KU_KEY_CERT_SIGN),
	V(X509_V_ERR_KU_KEY_CERT_SIGN_INVALID_FOR_NON_CA),
	V(X509_V_ERR_ISSUER_NAME_EMPTY),
	V(X509_V_ERR_SUBJECT_NAME_EMPTY),
	V(X509_V_ERR_MISSING_AUTHORITY_KEY_IDENTIFIER),
	V(X509_V_ERR_MISSING_SUBJECT_KEY_IDENTIFIER),
	V(X509_V_ERR_EMPTY_SUBJECT_ALT_NAME),
	V(X509_V_ERR_EMPTY_SUBJECT_SAN_NOT_CRITICAL),
	V(X509_V_ERR_CA_BCONS_NOT_CRITICAL),
	V(X509_V_ERR_AUTHORITY_KEY_IDENTIFIER_CRITICAL),
	V(X509_V_ERR_SUBJECT_KEY_IDENTIFIER_CRITICAL),
	V(X509_V_ERR_CA_CERT_MISSING_KEY_USAGE),
	V(X509_V_ERR_EXTENSIONS_REQUIRE_VERSION_3),
	V(X509_V_ERR_EC_KEY_EXPLICIT_PARAMS),
	{ 0, NULL, NULL },
};

/*
 * Return the X509_V_ERR code corresponding to the name of the constant.
 * See https://github.com/openssl/openssl/blob/master/include/openssl/x509_vfy.h.in
 * If not found, return -1
 */
int x509_v_err_str_to_int(const char *str)
{
	int i;

	for (i = 0; x509_v_codes[i].string; i++) {
		if (strcmp(str, x509_v_codes[i].string) == 0) {
			return x509_v_codes[i].code;
		}
	}

	return -1;
}

/*
 * Return the constant name corresponding to the X509_V_ERR code
 * See https://github.com/openssl/openssl/blob/master/include/openssl/x509_vfy.h.in
 * If not found, return NULL;
 */
const char *x509_v_err_int_to_str(int code)
{
	int i;

	if (code == -1)
		return NULL;

	for (i = 0; x509_v_codes[i].string; i++) {
		if (x509_v_codes[i].code == code) {
			return x509_v_codes[i].string;
		}
	}
	return NULL;
}

void init_x509_v_err_tab(void)
{
	int i;

	for (i = 0; x509_v_codes[i].string; i++) {
		/* either the macro exists or it's equal to its own name */
		if (strcmp(x509_v_codes[i].string, x509_v_codes[i].value) == 0)
			continue;
		x509_v_codes[i].code = atoi(x509_v_codes[i].value);
	}
}

INITCALL0(STG_REGISTER, init_x509_v_err_tab);


/*
 *  This function returns the number of seconds  elapsed
 *  since the Epoch, 1970-01-01 00:00:00 +0000 (UTC) and the
 *  date presented un ASN1_GENERALIZEDTIME.
 *
 *  In parsing error case, it returns -1.
 */
long asn1_generalizedtime_to_epoch(ASN1_GENERALIZEDTIME *d)
{
	long epoch;
	char *p, *end;
	const unsigned short month_offset[12] = {
		0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
	};
	unsigned long year, month;

	if (!d || (d->type != V_ASN1_GENERALIZEDTIME)) return -1;

	p = (char *)d->data;
	end = p + d->length;

	if (end - p < 4) return -1;
	year = 1000 * (p[0] - '0') + 100 * (p[1] - '0') + 10 * (p[2] - '0') + p[3] - '0';
	p += 4;
	if (end - p < 2) return -1;
	month = 10 * (p[0] - '0') + p[1] - '0';
	if (month < 1 || month > 12) return -1;
	/* Compute the number of seconds since 1 jan 1970 and the beginning of current month
	   We consider leap years and the current month (<marsh or not) */
	epoch = (  ((year - 1970) * 365)
		 + ((year - (month < 3)) / 4 - (year - (month < 3)) / 100 + (year - (month < 3)) / 400)
		 - ((1970 - 1) / 4 - (1970 - 1) / 100 + (1970 - 1) / 400)
		 + month_offset[month-1]
		) * 24 * 60 * 60;
	p += 2;
	if (end - p < 2) return -1;
	/* Add the number of seconds of completed days of current month */
	epoch += (10 * (p[0] - '0') + p[1] - '0' - 1) * 24 * 60 * 60;
	p += 2;
	if (end - p < 2) return -1;
	/* Add the completed hours of the current day */
	epoch += (10 * (p[0] - '0') + p[1] - '0') * 60 * 60;
	p += 2;
	if (end - p < 2) return -1;
	/* Add the completed minutes of the current hour */
	epoch += (10 * (p[0] - '0') + p[1] - '0') * 60;
	p += 2;
	if (p == end) return -1;
	/* Test if there is available seconds */
	if (p[0] < '0' || p[0] > '9')
		goto nosec;
	if (end - p < 2) return -1;
	/* Add the seconds of the current minute */
	epoch += 10 * (p[0] - '0') + p[1] - '0';
	p += 2;
	if (p == end) return -1;
	/* Ignore seconds float part if present */
	if (p[0] == '.') {
		do {
			if (++p == end) return -1;
		} while (p[0] >= '0' && p[0] <= '9');
	}

nosec:
	if (p[0] == 'Z') {
		if (end - p != 1) return -1;
		return epoch;
	}
	else if (p[0] == '+') {
		if (end - p != 5) return -1;
		/* Apply timezone offset */
		return epoch - ((10 * (p[1] - '0') + p[2] - '0') * 60 * 60 + (10 * (p[3] - '0') + p[4] - '0')) * 60;
	}
	else if (p[0] == '-') {
		if (end - p != 5) return -1;
		/* Apply timezone offset */
		return epoch + ((10 * (p[1] - '0') + p[2] - '0') * 60 * 60 + (10 * (p[3] - '0') + p[4] - '0')) * 60;
	}

	return -1;
}

/* Return the nofAfter value as as string extracted from an X509 certificate
 * The returned buffer is static and thread local.
 */
const char *x509_get_notafter(X509 *cert)
{
	BIO *bio = NULL;
	int write;
	static THREAD_LOCAL char buf[256];

	memset(buf, 0, sizeof(buf));

	if ((bio = BIO_new(BIO_s_mem())) ==  NULL)
		goto end;
	if (ASN1_TIME_print(bio, X509_getm_notAfter(cert)) == 0)
		goto end;
	write = BIO_read(bio, buf, sizeof(buf)-1);
	buf[write] = '\0';
	BIO_free(bio);

	return buf;

end:
	BIO_free(bio);
	return NULL;
}

/* Return the nofBefore value as as string extracted from an X509 certificate
 * The returned buffer is static and thread local.
 */
const char *x509_get_notbefore(X509 *cert)
{
	BIO *bio = NULL;
	int write;
	static THREAD_LOCAL char buf[256];

	memset(buf, 0, sizeof(buf));

	if ((bio = BIO_new(BIO_s_mem())) ==  NULL)
		goto end;
	if (ASN1_TIME_print(bio, X509_getm_notBefore(cert)) == 0)
		goto end;
	write = BIO_read(bio, buf, sizeof(buf)-1);
	buf[write] = '\0';
	BIO_free(bio);

	return buf;

end:
	BIO_free(bio);
	return NULL;
}

#ifdef HAVE_ASN1_TIME_TO_TM
/* Takes a ASN1_TIME and converts it into a time_t */
time_t ASN1_to_time_t(ASN1_TIME *asn1_time)
{
	struct tm tm;
	time_t ret = -1;

	if (ASN1_TIME_to_tm(asn1_time, &tm) == 0)
		goto error;

	ret  = my_timegm(&tm);
error:
	return ret;
}

/* return the notAfter date of a X509 certificate in a time_t format */
time_t x509_get_notafter_time_t(X509 *cert)
{
	time_t ret = -1;
	ASN1_TIME *asn1_time;

	if ((asn1_time = X509_getm_notAfter(cert)) == NULL)
		goto error;

	ret = ASN1_to_time_t(asn1_time);

error:
	return ret;
}

/* return the notBefore date of a X509 certificate in a time_t format */
time_t x509_get_notbefore_time_t(X509 *cert)
{
	time_t ret = -1;
	ASN1_TIME *asn1_time;

	if ((asn1_time = X509_getm_notBefore(cert)) == NULL)
		goto error;

	ret = ASN1_to_time_t(asn1_time);

error:
	return ret;
}
#endif

/* convert an OpenSSL NID to a NIST curves name */
const char *nid2nist(int nid)
{
	switch (nid) {
		case NID_X9_62_prime256v1: return "P-256";
		case NID_secp384r1:        return "P-384";
		case NID_secp521r1:        return "P-521";
		default:                   return NULL;
	}
}


/* https://datatracker.ietf.org/doc/html/rfc8446#section-4.2.3
 * https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-signaturescheme
 * Sigalg identifier to sigalg name table.
 * Some TLSv1.2 combinations are included as well to ease debugging. */
static struct sigalgs { const char *name; int sigalg; } sigalgs_list [] =
{
	/* RSASSA-PKCS1-v1_5 algorithms */
	{ "rsa_pkcs1_sha256", 0x0401 },
	{ "rsa_pkcs1_sha384", 0x0501 },
	{ "rsa_pkcs1_sha512", 0x0601 },

	/* ECDSA algorithms */
	{ "ecdsa_secp256r1_sha256", 0x0403 },
	{ "ecdsa_secp384r1_sha384", 0x0503 },
	{ "ecdsa_secp521r1_sha512", 0x0603 },

	/* RSASSA-PSS algorithms with public key OID rsaEncryption */
	{ "rsa_pss_rsae_sha256", 0x0804 },
	{ "rsa_pss_rsae_sha384", 0x0805 },
	{ "rsa_pss_rsae_sha512", 0x0806 },

	/* EdDSA algorithms */
	{ "ed25519", 0x0807 },
	{ "ed448", 0x0808 },

	/* RSASSA-PSS algorithms with public key OID RSASSA-PSS */
	{ "rsa_pss_pss_sha256", 0x0809 },
	{ "rsa_pss_pss_sha384", 0x080a },
	{ "rsa_pss_pss_sha512", 0x080b },

	/* Legacy algorithms */
	{ "rsa_pkcs1_sha1", 0x0201 },
	{ "ecdsa_sha1", 0x0203 },


	/* Other IANA codes */
	/* https://datatracker.ietf.org/doc/draft-davidben-tls13-pkcs1/00/ */
	{ "rsa_pkcs1_sha256_legacy", 0x0420 },
	{ "rsa_pkcs1_sha384_legacy", 0x0520 },
	{ "rsa_pkcs1_sha512_legacy", 0x0620 },

	/* https://datatracker.ietf.org/doc/draft-wang-tls-raw-public-key-with-ibc/02/ */
	{ "eccsi_sha256", 0x0704 },
	{ "iso_ibs1", 0x0705 },
	{ "iso_ibs2", 0x0706 },
	{ "iso_chinese_ibs", 0x0707 },

	/* RFC 8998 */
	{ "sm2sig_sm3", 0x0708 },

	/* RFC 9367 */
	{ "gostr34102012_256a", 0x0709 },
	{ "gostr34102012_256b", 0x070A },
	{ "gostr34102012_256c", 0x070B },
	{ "gostr34102012_256d", 0x070C },
	{ "gostr34102012_512a", 0x070D },
	{ "gostr34102012_512b", 0x070E },
	{ "gostr34102012_512c", 0x070F },

	/* RFC 8734 */
	{ "ecdsa_brainpoolP256r1tls13_sha256", 0x081A },
	{ "ecdsa_brainpoolP384r1tls13_sha384", 0x081B },
	{ "ecdsa_brainpoolP512r1tls13_sha512", 0x081C },


	/* TLSv1.2 backward compatibility */
	{ "dsa_sha256", 0x0402 },
	{ "dsa_sha384", 0x0502 },
	{ "dsa_sha512", 0x0602 },
	{ "dsa_sha224", 0x0302 },
	{ "dsa_sha1", 0x0202 },

	{ "ecdsa_sha224", 0x0303 },
	{ "ecdsa_sha1", 0x0203 },


	/* RFC 9189 */
	{ "gostr34102012_256_intrinsic", 0x0840 },
	{ "gostr34102012_512_intrinsic", 0x0841 },

	{ NULL, 0 }
};

/* Convert a signature algorithm identifier (2 bytes) to name */
const char *sigalg2str(int sigalg)
{
	struct sigalgs *item = sigalgs_list;

	while (item->name) {
		if (item->sigalg == sigalg)
			return item->name;

		++item;
	}

	return NULL;
}


/*
 * Like in x509_v_codes array, the following macros enable to use some NIDs that
 * can be undefined depending on the SSL library type or version. Those NIDs
 * will be converted to their numerical value when possible in
 * "init_curves_tab" function (called during init).
 */
#undef _Q
#define _Q(x) (#x)
#undef V
#define V(w, x, y, z) { .curve_id = w, .nid = -1, .nid_val_str = _Q(x), .name = y, .nist = z }

/*
 * Curve identifier to curve name mapping table. We use the actual identifiers
 * as defined in https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml#tls-parameters-8
 * as well as NIDs, special identifiers used in SSL libraries such as OpenSSL.
 * The names used are the standard SECG ones as well as the NIST ones.
 */
static struct curve {
	int curve_id;
	int nid;
	char *nid_val_str;
	const char *name;
	const char *nist;
} curves_list[] = {
	V( 1,      NID_sect163k1,                            "sect163k1",             "K-163"    ),
	V( 2,      NID_sect163r1,                            "sect163r1",             NULL       ),
	V( 3,      NID_sect163r2,                            "sect163r2",             "B-163"    ),
	V( 4,      NID_sect193r1,                            "sect193r1",             NULL       ),
	V( 5,      NID_sect193r2,                            "sect193r2",             NULL       ),
	V( 6,      NID_sect233k1,                            "sect233k1",             "K-233"    ),
	V( 7,      NID_sect233r1,                            "sect233r1",             "B-233"    ),
	V( 8,      NID_sect239k1,                            "sect239k1",             NULL       ),
	V( 9,      NID_sect283k1,                            "sect283k1",             "K-283"    ),
	V( 10,     NID_sect283r1,                            "sect283r1",             "B-283"    ),
	V( 11,     NID_sect409k1,                            "sect409k1",             "K-409"    ),
	V( 12,     NID_sect409r1,                            "sect409r1",             "B-409"    ),
	V( 13,     NID_sect571k1,                            "sect571k1",             "K-571"    ),
	V( 14,     NID_sect571r1,                            "sect571r1",             "B-571"    ),
	V( 15,     NID_secp160k1,                            "secp160k1",             NULL       ),
	V( 16,     NID_secp160r1,                            "secp160r1",             NULL       ),
	V( 17,     NID_secp160r2,                            "secp160r2",             NULL       ),
	V( 18,     NID_secp192k1,                            "secp192k1",             NULL       ),
	V( 19,     NID_X9_62_prime192v1,                     "secp192r1",             "P-192"    ),
	V( 20,     NID_secp224k1,                            "secp224k1",             NULL       ),
	V( 21,     NID_secp224r1,                            "secp224r1",             "P-224"    ),
	V( 22,     NID_secp256k1,                            "secp256k1",             NULL       ),
	V( 23,     NID_X9_62_prime256v1,                     "secp256r1",             "P-256"    ),
	V( 24,     NID_secp384r1,                            "secp384r1",             "P-384"    ),
	V( 25,     NID_secp521r1,                            "secp521r1",             "P-521"    ),
	V( 26,     NID_brainpoolP256r1,                      "brainpoolP256r1",       NULL       ),
	V( 27,     NID_brainpoolP384r1,                      "brainpoolP384r1",       NULL       ),
	V( 28,     NID_brainpoolP512r1,                      "brainpoolP512r1",       NULL       ),
	V( 29,     EVP_PKEY_X25519,                          "ecdh_x25519",           NULL       ),
	V( 30,     EVP_PKEY_X448,                            "ecdh_x448",             NULL       ),
	V( 31,     NID_brainpoolP256r1tls13,                 "brainpoolP256r1tls13",  NULL       ),
	V( 32,     NID_brainpoolP384r1tls13,                 "brainpoolP384r1tls13",  NULL       ),
	V( 33,     NID_brainpoolP512r1tls13,                 "brainpoolP512r1tls13",  NULL       ),
	V( 34,     NID_id_tc26_gost_3410_2012_256_paramSetA, "GC256A",                NULL       ),
	V( 35,     NID_id_tc26_gost_3410_2012_256_paramSetB, "GC256B",                NULL       ),
	V( 36,     NID_id_tc26_gost_3410_2012_256_paramSetC, "GC256C",                NULL       ),
	V( 37,     NID_id_tc26_gost_3410_2012_256_paramSetD, "GC256D",                NULL       ),
	V( 38,     NID_id_tc26_gost_3410_2012_512_paramSetA, "GC512A",                NULL       ),
	V( 39,     NID_id_tc26_gost_3410_2012_512_paramSetB, "GC512B",                NULL       ),
	V( 40,     NID_id_tc26_gost_3410_2012_512_paramSetC, "GC512C",                NULL       ),
	V( 256,    NID_ffdhe2048,                            "ffdhe2048",             NULL       ),
	V( 257,    NID_ffdhe3072,                            "ffdhe3072",             NULL       ),
	V( 258,    NID_ffdhe4096,                            "ffdhe4096",             NULL       ),
	V( 259,    NID_ffdhe6144,                            "ffdhe6144",             NULL       ),
	V( 260,    NID_ffdhe8192,                            "ffdhe8192",             NULL       ),


	/* The following curves are defined in the IANA list as well as in an
	 * OpenSSL internal array but they don't have any corresponding NID.
	 */
	V( 25497,  -1,                                       "X25519Kyber768Draft00",           NULL ),
	V( 25498,  -1,                                       "SecP256r1Kyber768Draft00",        NULL ),
	V( 0xFF01, -1,                                       "arbitrary_explicit_prime_curves", NULL ),
	V( 0xFF02, -1,                                       "arbitrary_explicit_char2_curves", NULL ),
	{ 0, 0, NULL, NULL, NULL }
};

void init_curves_tab(void)
{
	int i;

	for (i = 0; curves_list[i].nid_val_str; i++) {
		char *endptr = NULL;
		long value = 0;

		errno = 0;
		value = strtol(curves_list[i].nid_val_str, &endptr, 10);

		if (!errno && endptr > curves_list[i].nid_val_str)
			curves_list[i].nid = value;
	}
}

INITCALL0(STG_REGISTER, init_curves_tab);

/* Convert a curve identifier (2 bytes) to name */
const char *curveid2str(int curve_id)
{
	struct curve *item = curves_list;

	while (item->name) {
		if (item->curve_id == curve_id)
			return item->name;

		++item;
	}

	return NULL;
}

/* convert a curves name to a openssl NID */
int curves2nid(const char *curve)
{
	struct curve *curves = curves_list;

	while (curves->curve_id) {
		if ((curves->name && strcmp(curve, curves->name) == 0) ||
		    (curves->nist && strcmp(curve, curves->nist) == 0))
			return curves->nid;
		curves++;
	}
	return -1;
}

