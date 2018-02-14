/*
   BAREOS® - Backup Archiving REcovery Open Sourced

   Copyright (C) 2005-2010 Free Software Foundation Europe e.V.
   Copyright (C) 2014-2018 Bareos GmbH & Co. KG

   This program is Free Software; you can redistribute it and/or
   modify it under the terms of version three of the GNU Affero General Public
   License as published by the Free Software Foundation and included
   in the file LICENSE.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
   Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301, USA.
*/
/*
 * tls_openssl.c TLS support functions when using OPENSSL backend.
 *
 * Author: Landon Fuller <landonf@threerings.net>
 */

#include "bareos.h"
#include <assert.h>

static inline int cval(char c) {
   if (c >= 'a')
      return c - 'a' + 0x0a;
   if (c >= 'A')
      return c - 'A' + 0x0a;
   return c - '0';
}

/* return value: number of bytes in out, <=0 if error */
int hex2bin(char *str, unsigned char *out, unsigned int max_out_len) {
   unsigned int i;
   for (i = 0; str[i] && str[i + 1] && i < max_out_len * 2; i += 2) {
      if (!isxdigit(str[i]) && !isxdigit(str[i + 1])) {
         return -1;
      }
      out[i / 2] = (cval(str[i]) << 4) + cval(str[i + 1]);
   }

   return i / 2;
}


#if defined(HAVE_TLS) && defined(HAVE_OPENSSL)

#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <openssl/err.h>
#include <openssl/asn1.h>
#include <openssl/asn1t.h>

/* tls_t */
#include "parse_conf.h"

/*
 * No anonymous ciphers, no <128 bit ciphers, no export ciphers, no MD5 ciphers
 */
#define TLS_DEFAULT_CIPHERS "ALL:!ADH:!LOW:!EXP:!MD5:@STRENGTH"

#define MAX_CRLS 16

/**
 * stores the tls_psk_pair to the corresponding ssl context
 */
static std::map<SSL_CTX *, sharedPskCredentials> psk_server_credentials;

/**
 * stores the tls_psk_pair to the corresponding ssl context
 */
static std::map<SSL_CTX *, sharedPskCredentials> psk_client_credentials;


/*
 * TLS Context Structures
 */
class TLS_Context {
 public:
   SSL_CTX *openssl;
   CRYPTO_PEM_PASSWD_CB *pem_callback;
   const void *pem_userdata;
   TLS_Context() : openssl(nullptr), pem_callback(nullptr), pem_userdata(nullptr) {
    Dmsg0(100, "Construct TLS_Context\n");
   }

   ~TLS_Context() {
      Dmsg0(100, "Destruct TLS_Context\n");
      if (openssl != nullptr) {
         try {
            psk_server_credentials.erase(openssl);
         } catch (const std::out_of_range &exception) {
            // no credentials found -> nothing to do.
         }
         try {
            psk_client_credentials.erase(openssl);
         } catch (const std::out_of_range &exception) {
            // no credentials found -> nothing to do.
         }
         SSL_CTX_free(openssl);
      }
   }
};

class TLS_Connection {
   std::shared_ptr<TLS_Context> tls_ctx_;
   SSL *openssl_;

 public:
   std::shared_ptr<TLS_Context> GetTls() { return tls_ctx_; }
   SSL *GetSsl() { return openssl_; }

   TLS_Connection(std::shared_ptr<TLS_Context> tls_ctx,
                  int fd,
                  unsigned int (*psk_client_callback)(SSL *ssl,
                                                      const char *hint,
                                                      char *identity,
                                                      unsigned int max_identity_len,
                                                      unsigned char *psk,
                                                      unsigned int max_psk_len),
                  unsigned int (*psk_server_callback)(
                      SSL *ssl, const char *identity, unsigned char *psk, unsigned int max_psk_len))
       : tls_ctx_(tls_ctx), openssl_(nullptr) {
      /*
       * Create a new BIO and assign the fd.
       * The caller will remain responsible for closing the associated fd
       */
      BIO *bio = BIO_new(BIO_s_socket());

      if (!bio) {
         /* Not likely, but never say never */
         openssl_post_errors(M_FATAL, _("Error creating file descriptor-based BIO"));
         throw;
      }

      BIO_set_fd(bio, fd, BIO_NOCLOSE);

      /* Create the SSL object and attach the socket BIO */
      openssl_ = SSL_new(tls_ctx_->openssl);
      if (openssl_ == NULL) {
         /* Not likely, but never say never */
         openssl_post_errors(M_FATAL, _("Error creating new SSL object"));

         BIO_free(bio);
         SSL_free(openssl_);
         throw;
      }

      SSL_CTX_set_psk_client_callback(tls_ctx_->openssl, psk_client_callback);
      SSL_CTX_set_psk_server_callback(tls_ctx_->openssl, psk_server_callback);

      SSL_set_bio(openssl_, bio, bio);

      /* Non-blocking partial writes */
      SSL_set_mode(openssl_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
   }

   ~TLS_Connection() {
      Dmsg0(100, "Destruct TLS_Connection\n");
      SSL_free(openssl_);
   }
};

#if (OPENSSL_VERSION_NUMBER >= 0x00907000L) && (OPENSSL_VERSION_NUMBER < 0x10100000L)
struct TLS_CRL_Reload_Context {
   time_t mtime;
   char *crl_file_name;
   X509_CRL *crls[MAX_CRLS];
};

/*
 * Automatic Certificate Revocation List reload logic.
 */
static int crl_reloader_new(X509_LOOKUP *ctx)
{
   TLS_CRL_Reload_Context *data;

   data = (TLS_CRL_Reload_Context *)malloc(sizeof(TLS_CRL_Reload_Context));
   memset(data, 0, sizeof(TLS_CRL_Reload_Context));

   ctx->method_data = (char *)data;
   return 1;
}

static void crl_reloader_free(X509_LOOKUP *ctx)
{
   int cnt;
   TLS_CRL_Reload_Context *data;

   if (ctx->method_data) {
      data = (TLS_CRL_Reload_Context *)ctx->method_data;

      if (data->crl_file_name) {
         free(data->crl_file_name);
      }

      for (cnt = 0; cnt < MAX_CRLS; cnt++) {
         if (data->crls[cnt]) {
            X509_CRL_free(data->crls[cnt]);
         }
      }

      free(data);
      ctx->method_data = NULL;
   }
}

/*
 * Load the new content from a Certificate Revocation List (CRL).
 */
static int crl_reloader_reload_file(X509_LOOKUP *ctx)
{
   int cnt, ok = 0;
   struct stat st;
   BIO *in = NULL;
   TLS_CRL_Reload_Context *data;

   data = (TLS_CRL_Reload_Context *)ctx->method_data;
   if (!data->crl_file_name) {
      goto bail_out;
   }

   if (stat(data->crl_file_name, &st) != 0) {
      goto bail_out;
   }

   in = BIO_new_file(data->crl_file_name, "r");
   if (!in) {
      goto bail_out;
   }

   /*
    * Load a maximum of MAX_CRLS Certificate Revocation Lists.
    */
   data->mtime = st.st_mtime;
   for (cnt = 0; cnt < MAX_CRLS; cnt++) {
      X509_CRL *crl;

      if ((crl = PEM_read_bio_X509_CRL(in, NULL, NULL, NULL)) == NULL) {
         if (cnt == 0) {
            /*
             * We try to read multiple times only the first is fatal.
             */
            goto bail_out;
         } else {
            break;
         }
      }

      if (data->crls[cnt]) {
         X509_CRL_free(data->crls[cnt]);
      }
      data->crls[cnt] = crl;
   }

   /*
    * Clear the other slots.
    */
   while (++cnt < MAX_CRLS) {
      if (data->crls[cnt]) {
         X509_CRL_free(data->crls[cnt]);
         data->crls[cnt] = NULL;
      }
   }

   ok = 1;

bail_out:
   if (in) {
      BIO_free(in);
   }
   return ok;
}

/*
 * See if the data in the Certificate Revocation List (CRL) is newer then we loaded before.
 */
static int crl_reloader_reload_if_newer(X509_LOOKUP *ctx)
{
   int ok = 0;
   TLS_CRL_Reload_Context *data;
   struct stat st;

   data = (TLS_CRL_Reload_Context *)ctx->method_data;
   if (!data->crl_file_name) {
      return ok;
   }

   if (stat(data->crl_file_name, &st) != 0) {
      return ok;
   }

   if (st.st_mtime > data->mtime) {
      ok = crl_reloader_reload_file(ctx);
      if (!ok) {
         goto bail_out;
      }
   }
   ok = 1;

bail_out:
   return ok;
}

/*
 * Load the data from a Certificate Revocation List (CRL) into memory.
 */
static int crl_reloader_file_load(X509_LOOKUP *ctx, const char *argp)
{
   int ok = 0;
   TLS_CRL_Reload_Context *data;

   data = (TLS_CRL_Reload_Context *)ctx->method_data;
   if (data->crl_file_name) {
      free(data->crl_file_name);
   }
   data->crl_file_name = bstrdup(argp);

   ok = crl_reloader_reload_file(ctx);
   if (!ok) {
      goto bail_out;
   }
   ok = 1;

bail_out:
   return ok;
}

static int crl_reloader_ctrl(X509_LOOKUP *ctx, int cmd, const char *argp, long argl, char **ret)
{
   int ok = 0;

   switch (cmd) {
   case X509_L_FILE_LOAD:
      ok = crl_reloader_file_load(ctx, argp);
      break;
   default:
      break;
   }

   return ok;
}

/*
 * Check if a CRL entry is expired.
 */
static int crl_entry_expired(X509_CRL *crl)
{
   int lastUpdate, nextUpdate;

   if (!crl) {
      return 0;
   }

   lastUpdate = X509_cmp_current_time(X509_CRL_get_lastUpdate(crl));
   nextUpdate = X509_cmp_current_time(X509_CRL_get_nextUpdate(crl));

   if (lastUpdate < 0 && nextUpdate > 0) {
      return 0;
   }

   return 1;
}

/*
 * Retrieve a CRL entry by Subject.
 */
static int crl_reloader_get_by_subject(X509_LOOKUP *ctx, int type, X509_NAME *name, X509_OBJECT *ret)
{
   int cnt, ok = 0;
   TLS_CRL_Reload_Context *data = NULL;

   if (type != X509_LU_CRL) {
      return ok;
   }

   data = (TLS_CRL_Reload_Context *)ctx->method_data;
   if (!data->crls[0]) {
      return ok;
   }

   ret->type = 0;
   ret->data.crl = NULL;
   for (cnt = 0; cnt < MAX_CRLS; cnt++) {
      if (crl_entry_expired(data->crls[cnt]) && !crl_reloader_reload_if_newer(ctx)) {
         goto bail_out;
      }

      if (X509_NAME_cmp(data->crls[cnt]->crl->issuer, name)) {
         continue;
      }

      ret->type = type;
      ret->data.crl = data->crls[cnt];
      ok = 1;
      break;
   }

   return ok;

bail_out:
   return ok;
}

static int load_new_crl_file(X509_LOOKUP *lu, const char *fname)
{
   int ok = 0;

   if (!fname) {
      return ok;
   }
   ok = X509_LOOKUP_ctrl(lu, X509_L_FILE_LOAD, fname, 0, NULL);

   return ok;
}

static X509_LOOKUP_METHOD x509_crl_reloader = {
   "CRL file reloader",
   crl_reloader_new,            /* new */
   crl_reloader_free,           /* free */
   NULL,                        /* init */
   NULL,                        /* shutdown */
   crl_reloader_ctrl,           /* ctrl */
   crl_reloader_get_by_subject, /* get_by_subject */
   NULL,                        /* get_by_issuer_serial */
   NULL,                        /* get_by_fingerprint */
   NULL                         /* get_by_alias */
};

static X509_LOOKUP_METHOD *X509_LOOKUP_crl_reloader(void)
{
   return (&x509_crl_reloader);
}
#endif /* (OPENSSL_VERSION_NUMBER > 0x00907000L) */

/*
 * OpenSSL certificate verification callback.
 * OpenSSL has already performed internal certificate verification.
 * We just report any errors that occured.
 */
static int openssl_verify_peer(int ok, X509_STORE_CTX *store)
{
   if (!ok) {
      X509 *cert = X509_STORE_CTX_get_current_cert(store);
      int depth = X509_STORE_CTX_get_error_depth(store);
      int err = X509_STORE_CTX_get_error(store);
      char issuer[256];
      char subject[256];

      X509_NAME_oneline(X509_get_issuer_name(cert), issuer, 256);
      X509_NAME_oneline(X509_get_subject_name(cert), subject, 256);

      Jmsg5(NULL, M_ERROR, 0, _("Error with certificate at depth: %d, issuer = %s,"
            " subject = %s, ERR=%d:%s\n"), depth, issuer,
              subject, err, X509_verify_cert_error_string(err));

   }

   return ok;
}

/*
 * Dispatch user PEM encryption callbacks
 */
static int tls_pem_callback_dispatch(char *buf, int size, int rwflag,
                                     void *userdata) {
   TLS_CONTEXT *ctx = (TLS_CONTEXT *)userdata;
   return (ctx->pem_callback(buf, size, ctx->pem_userdata));
}

static unsigned int psk_server_cb(SSL *ssl,
                                  const char *identity,
                                  unsigned char *psk,
                                  unsigned int max_psk_len) {
   unsigned int result = 0;

   SSL_CTX *ctx = SSL_get_SSL_CTX(ssl);
   Dmsg1(100, "psk_server_cb. identitiy: %s.\n", identity);

   if (NULL != ctx) {
      try {
         std::shared_ptr<PskCredentials> credentials = psk_server_credentials.at(ctx);
         /* okay. we found the appropriate psk identity pair.
          * Now let's check if the given identity is the same and
          * provide the psk.
          */
         if (credentials->get_identity() == std::string(identity)) {
            int psklen = bsnprintf((char *)psk, max_psk_len, "%s", credentials->get_psk().c_str());
            result = (psklen < 0) ? result = 0 : result = psklen;
            Dmsg1(100, "psk_server_cb. psk: %s.\n", psk);
        }
         return result;
      } catch (const std::out_of_range /* &exception */) {
         // ssl context unknown
         Dmsg0(100, "Error, TLS-PSK credentials not found.\n");
         return 0;
      }
   }
   Dmsg0(100, "Error, SSL_CTX not set.\n");
   return result;
}

static unsigned int psk_client_cb(SSL *ssl,
                                  const char * /*hint*/,
                                  char *identity,
                                  unsigned int max_identity_len,
                                  unsigned char *psk,
                                  unsigned int max_psk_len) {

   SSL_CTX *ctx = SSL_get_SSL_CTX(ssl);

   if (NULL != ctx) {
      try {
         sharedPskCredentials credentials = psk_client_credentials.at(ctx);
         /* okay. we found the appropriate psk identity pair.
          * Now let's check if the given identity is the same and
          * provide the psk.
          */
         unsigned int ret =
             bsnprintf(identity, max_identity_len, "%s", credentials->get_identity().c_str());

         if (ret < 0 || (unsigned int)ret > max_identity_len) {
            Dmsg0(100, "Error, identify too long\n");
            return 0;
         }
         Dmsg1(100, "psk_client_cb. identity: %s.\n", identity);

         ret = bsnprintf((char *)psk, max_psk_len, "%s", credentials->get_psk().c_str());
         if (ret < 0 || (unsigned int)ret > max_psk_len) {
            Dmsg0(100, "Error, psk too long\n");
            return 0;
         }
         Dmsg1(100, "psk_client_cb. psk: %s.\n", psk);

         return ret;
      } catch (const std::out_of_range &exception) {
         // ssl context unknown
         Dmsg0(100, "Error, TLS-PSK CALLBACK not set.\n");
         return 0;
      }
   }

   Dmsg0(100, "Error, SSL_CTX not set.\n");
   return 0;
}

/*
 * Create a new TLS_CONTEXT instance for use with tls-psk.
 *
 * Returns: Pointer to TLS_CONTEXT instance on success
 *          NULL on failure;
 */
static std::shared_ptr<TLS_CONTEXT> new_tls_psk_context(const char *cipherlist) {
   std::shared_ptr<TLS_CONTEXT> ctx;

   ctx = std::make_shared<TLS_CONTEXT>();

#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
   ctx->openssl = SSL_CTX_new(TLS_method());
#else
   ctx->openssl = SSL_CTX_new(SSLv23_method());
#endif
   if (!ctx->openssl) {
      openssl_post_errors(M_FATAL, _("Error initializing SSL context"));
      goto err;
   }

   /*
    * Enable all Bug Workarounds
    */
   SSL_CTX_set_options(ctx->openssl, SSL_OP_ALL);

#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
   /*
    * Disallow broken sslv2 and sslv3.
    */
   SSL_CTX_set_options(ctx->openssl, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
#endif

   if (!cipherlist) {
      cipherlist = TLS_DEFAULT_CIPHERS;
   }

   if (SSL_CTX_set_cipher_list(ctx->openssl, cipherlist) != 1) {
      Jmsg0(NULL, M_ERROR, 0, _("Error setting cipher list, no valid ciphers available\n"));
      goto err;
   }

   return ctx;

err:
   /*
    * Clean up after ourselves
    */
   if (ctx->openssl) {
      SSL_CTX_free(ctx->openssl);
   }

   return nullptr;
}

static std::shared_ptr<TLS_CONTEXT> new_tls_psk_client_context(
    const char *cipherlist, std::shared_ptr<PskCredentials> credentials) {
   Dmsg1(50, "Preparing TLS_PSK client context for identity %s\n", credentials->get_identity().c_str());
   std::shared_ptr<TLS_CONTEXT> tls_context = new_tls_psk_context(cipherlist);
   if (NULL != credentials) {
      psk_client_credentials[tls_context->openssl] = credentials;
      SSL_CTX_set_psk_client_callback(tls_context->openssl, psk_client_cb);
   }

   return tls_context;
}

static std::shared_ptr<TLS_CONTEXT> new_tls_psk_server_context(
    const char *cipherlist, std::shared_ptr<PskCredentials> credentials) {
   Dmsg1(50, "Preparing TLS_PSK server context for identity %s\n", credentials->get_identity().c_str());
   std::shared_ptr<TLS_CONTEXT> tls_context = new_tls_psk_context(cipherlist);

   if (NULL != credentials) {
      psk_server_credentials[tls_context->openssl] = credentials;
      SSL_CTX_set_psk_server_callback(tls_context->openssl, psk_server_cb);
   }

   return tls_context;
}

/*
 * Create a new TLS_CONTEXT instance.
 *
 * Returns: Pointer to TLS_CONTEXT instance on success
 *          NULL on failure;
 */
static  std::shared_ptr<TLS_CONTEXT> new_tls_context(const char *ca_certfile, const char *ca_certdir,
                             const char *crlfile, const char *certfile,
                             const char *keyfile,
                             CRYPTO_PEM_PASSWD_CB *pem_callback,
                             const void *pem_userdata, const char *dhfile,
                             const char *cipherlist, bool verify_peer) {
   BIO *bio;
   DH *dh;

   std::shared_ptr<TLS_CONTEXT> ctx = std::make_shared<TLS_CONTEXT>();

   /*
    * Allocate our OpenSSL Context
    * We allow tls 1.2. 1.1 and 1.0
    */
#if (OPENSSL_VERSION_NUMBER >= 0x10100000L)
   ctx->openssl = SSL_CTX_new(TLS_method());
#else
   ctx->openssl = SSL_CTX_new(SSLv23_method());
#endif
   if (!ctx->openssl) {
      openssl_post_errors(M_FATAL, _("Error initializing SSL context"));
      goto err;
   }

   /*
    * Enable all Bug Workarounds
    */
   SSL_CTX_set_options(ctx->openssl, SSL_OP_ALL);

#if (OPENSSL_VERSION_NUMBER < 0x10100000L)
   /*
    * Disallow broken sslv2 and sslv3.
    */
   SSL_CTX_set_options(ctx->openssl, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);
#endif

   /*
    * Set up pem encryption callback
    */
   if (pem_callback) {
      ctx->pem_callback = pem_callback;
      ctx->pem_userdata = pem_userdata;
   } else {
      ctx->pem_callback = crypto_default_pem_callback;
      ctx->pem_userdata = NULL;
   }

   SSL_CTX_set_default_passwd_cb(ctx->openssl, tls_pem_callback_dispatch);
   SSL_CTX_set_default_passwd_cb_userdata(ctx->openssl, reinterpret_cast<void *>(ctx.get()));

   /*
    * Set certificate verification paths. This requires that at least one value be non-NULL
    */
   if (ca_certfile || ca_certdir) {
      if (!SSL_CTX_load_verify_locations(ctx->openssl, ca_certfile, ca_certdir)) {
         openssl_post_errors(M_FATAL, _("Error loading certificate verification stores"));
         goto err;
      }
   } else if (verify_peer) {
      /* At least one CA is required for peer verification */
      Jmsg0(NULL, M_ERROR, 0, _("Either a certificate file or a directory must be"
                         " specified as a verification store\n"));
      goto err;
   }

#if (OPENSSL_VERSION_NUMBER >= 0x00907000L)  && (OPENSSL_VERSION_NUMBER < 0x10100000L)
   /*
    * Set certificate revocation list.
    */
   if (crlfile) {
      X509_STORE *store;
      X509_LOOKUP *lookup;

      store = SSL_CTX_get_cert_store(ctx->openssl);
      if (!store) {
         openssl_post_errors(M_FATAL, _("Error loading revocation list file"));
         goto err;
      }

      lookup = X509_STORE_add_lookup(store, X509_LOOKUP_crl_reloader());
      if (!lookup) {
         openssl_post_errors(M_FATAL, _("Error loading revocation list file"));
         goto err;
      }

      if (!load_new_crl_file(lookup, (char *)crlfile)) {
         openssl_post_errors(M_FATAL, _("Error loading revocation list file"));
         goto err;
      }

      X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
   }
#endif

   /*
    * Load our certificate file, if available. This file may also contain a
    * private key, though this usage is somewhat unusual.
    */
   if (certfile) {
      if (!SSL_CTX_use_certificate_chain_file(ctx->openssl, certfile)) {
         openssl_post_errors(M_FATAL, _("Error loading certificate file"));
         goto err;
      }
   }

   /*
    * Load our private key.
    */
   if (keyfile) {
      if (!SSL_CTX_use_PrivateKey_file(ctx->openssl, keyfile, SSL_FILETYPE_PEM)) {
         openssl_post_errors(M_FATAL, _("Error loading private key"));
         goto err;
      }
   }

   /*
    * Load Diffie-Hellman Parameters.
    */
   if (dhfile) {
      if (!(bio = BIO_new_file(dhfile, "r"))) {
         openssl_post_errors(M_FATAL, _("Unable to open DH parameters file"));
         goto err;
      }
      dh = PEM_read_bio_DHparams(bio, NULL, NULL, NULL);
      BIO_free(bio);
      if (!dh) {
         openssl_post_errors(M_FATAL, _("Unable to load DH parameters from specified file"));
         goto err;
      }
      if (!SSL_CTX_set_tmp_dh(ctx->openssl, dh)) {
         openssl_post_errors(M_FATAL, _("Failed to set TLS Diffie-Hellman parameters"));
         DH_free(dh);
         goto err;
      }

      /*
       * Enable Single-Use DH for Ephemeral Keying
       */
      SSL_CTX_set_options(ctx->openssl, SSL_OP_SINGLE_DH_USE);
   }

   if (!cipherlist) {
      cipherlist = TLS_DEFAULT_CIPHERS;
   }

   if (SSL_CTX_set_cipher_list(ctx->openssl, cipherlist) != 1) {
      Jmsg0(NULL, M_ERROR, 0,
             _("Error setting cipher list, no valid ciphers available\n"));
      goto err;
   }

   /*
    * Verify Peer Certificate
    */
   if (verify_peer) {
      /*
       * SSL_VERIFY_FAIL_IF_NO_PEER_CERT has no effect in client mode
       */
      SSL_CTX_set_verify(ctx->openssl,
                         SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                         openssl_verify_peer);
   } else {
      SSL_CTX_set_verify(ctx->openssl,
                         SSL_VERIFY_NONE,
                         NULL);
   }

   return ctx;

err:
   return nullptr;
}

std::shared_ptr<TLS_CONTEXT> tls_cert_t::CreateClientContext(
    std::shared_ptr<PskCredentials> /* credentials */) const {
   return new_tls_context((!ca_certfile || ca_certfile->empty()) ? nullptr : ca_certfile->c_str(),
                          (!ca_certdir || ca_certdir->empty()) ? nullptr : ca_certdir->c_str(),
                          (!crlfile || crlfile->empty()) ? nullptr : crlfile->c_str(),
                          (!certfile || certfile->empty()) ? nullptr : certfile->c_str(),
                          (!keyfile || keyfile->empty()) ? nullptr : keyfile->c_str(),
                          tls_pem_callback,
                          (!pem_message || pem_message->empty()) ? nullptr : pem_message->c_str(),
                          nullptr,
                          (!cipherlist || cipherlist->empty()) ? nullptr : cipherlist->c_str(),
                          verify_peer);
}

std::shared_ptr<TLS_CONTEXT> tls_cert_t::CreateServerContext(
    std::shared_ptr<PskCredentials> /* credentials */) const {
   return new_tls_context((!ca_certfile || ca_certfile->empty()) ? nullptr : ca_certfile->c_str(),
                          (!ca_certdir || ca_certdir->empty()) ? nullptr : ca_certdir->c_str(),
                          (!crlfile || crlfile->empty()) ? nullptr : crlfile->c_str(),
                          (!certfile || certfile->empty()) ? nullptr : certfile->c_str(),
                          (!keyfile || keyfile->empty()) ? nullptr : keyfile->c_str(),
                          tls_pem_callback,
                          (!pem_message || pem_message->empty()) ? nullptr : pem_message->c_str(),
                          nullptr,
                          (!cipherlist || cipherlist->empty()) ? nullptr : cipherlist->c_str(),
                          verify_peer);
}

bool tls_cert_t::enabled(u_int32_t policy) {
   return (policy >> tls_cert_t::policy_offset) & BNET_TLS_ENABLED == BNET_TLS_ENABLED;
}

bool tls_cert_t::required(u_int32_t policy) {
   return (policy >> tls_cert_t::policy_offset) & BNET_TLS_REQUIRED == BNET_TLS_REQUIRED;
}

bool tls_psk_t::enabled(u_int32_t policy) {
   return (policy >> tls_psk_t::policy_offset) & BNET_TLS_ENABLED == BNET_TLS_ENABLED;
}

bool tls_psk_t::required(u_int32_t policy) {
   return (policy >> tls_psk_t::policy_offset) & BNET_TLS_REQUIRED == BNET_TLS_REQUIRED;
}

std::shared_ptr<TLS_CONTEXT> tls_psk_t::CreateClientContext(
    std::shared_ptr<PskCredentials> credentials) const {
   return new_tls_psk_client_context(cipherlist, credentials);
}

std::shared_ptr<TLS_CONTEXT> tls_psk_t::CreateServerContext(
    std::shared_ptr<PskCredentials> credentials) const {
   return new_tls_psk_server_context(cipherlist, credentials);
}

/*
 * Free TLS_CONTEXT instance
 */
void free_tls_context(std::shared_ptr<TLS_CONTEXT> &ctx) {
   psk_server_credentials.erase(ctx->openssl);
   psk_client_credentials.erase(ctx->openssl);
   SSL_CTX_free(ctx->openssl);
   ctx.reset();
}

/**
 * Get connection cipher info and log it into joblog
 */
void tls_log_conninfo(JCR *jcr, TLS_CONNECTION *tls_conn, const char *host, int port, const char *who) {
   if (tls_conn == nullptr) {
         Qmsg(jcr, M_INFO, 0, _("Cleartext connection to %s at %s:%d established\n"), who, host, port);
   } else {
      SSL *ssl                 = tls_conn->GetSsl();
      const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
      const char *cipher_name  = NULL;

      if (cipher) {
         cipher_name = SSL_CIPHER_get_name(cipher);
         Qmsg(jcr, M_INFO, 0, _("Secure connection to %s at %s:%d with cipher %s established\n"), who, host, port, cipher_name);
      } else {
         Qmsg3(jcr, M_WARNING, 0, _("Secure connection to %s at %s:%d with UNKNOWN cipher established\n"), who, host, port);
      }
   }
}

/*
 * Verifies a list of common names against the certificate commonName attribute.
 *
 * Returns: true on success
 *          false on failure
 */
bool tls_postconnect_verify_cn(JCR *jcr, TLS_CONNECTION *tls_conn, alist *verify_list)
{
   SSL *ssl = tls_conn->GetSsl();
   X509 *cert;
   X509_NAME *subject;
   bool auth_success = false;
   char data[256];

   /*
    * Check if peer provided a certificate
    */
   if (!(cert = SSL_get_peer_certificate(ssl))) {
      Qmsg0(jcr, M_ERROR, 0, _("Peer failed to present a TLS certificate\n"));
      return false;
   }

   if ((subject = X509_get_subject_name(cert)) != NULL) {
      if (X509_NAME_get_text_by_NID(subject, NID_commonName, data, sizeof(data)) > 0) {
         char *cn;
         data[255] = 0; /* NULL terminate data */

         /*
          * Try all the CNs in the list
          */
         foreach_alist(cn, verify_list) {
            Dmsg2(120, "comparing CNs: cert-cn=%s, allowed-cn=%s\n", data, cn);
            if (bstrcasecmp(data, cn)) {
               auth_success = true;
            }
         }
      }
   }

   X509_free(cert);
   return auth_success;
}

/*
 * Verifies a peer's hostname against the subjectAltName and commonName attributes.
 *
 * Returns: true on success
 *          false on failure
 */
bool tls_postconnect_verify_host(JCR *jcr, TLS_CONNECTION *tls_conn, const char *host)
{
   int i, j;
   int extensions;
   int cnLastPos = -1;
   X509 *cert;
   X509_NAME *subject;
   X509_NAME_ENTRY *neCN;
   ASN1_STRING *asn1CN;
   SSL *ssl = tls_conn->GetSsl();
   bool auth_success = false;

   /*
    * Check if peer provided a certificate
    */
   if (!(cert = SSL_get_peer_certificate(ssl))) {
      Qmsg1(jcr, M_ERROR, 0,
            _("Peer %s failed to present a TLS certificate\n"), host);
      return false;
   }

   /*
    * Check subjectAltName extensions first
    */
   if ((extensions = X509_get_ext_count(cert)) > 0) {
      for (i = 0; i < extensions; i++) {
         X509_EXTENSION *ext;
         const char *extname;

         ext = X509_get_ext(cert, i);
         extname = OBJ_nid2sn(OBJ_obj2nid(X509_EXTENSION_get_object(ext)));

         if (bstrcmp(extname, "subjectAltName")) {
#if (OPENSSL_VERSION_NUMBER >= 0x10000000L)
            const X509V3_EXT_METHOD *method;
#else
            X509V3_EXT_METHOD *method;
#endif
            STACK_OF(CONF_VALUE) *val;
            CONF_VALUE *nval;
            void *extstr = NULL;
#if (OPENSSL_VERSION_NUMBER >= 0x0090800FL)
            const unsigned char *ext_value_data;
#else
            unsigned char *ext_value_data;
#endif

            /*
             * Get x509 extension method structure
             */
            if (!(method = X509V3_EXT_get(ext))) {
               break;
            }

            ext_value_data = X509_EXTENSION_get_data(ext)->data;

#if (OPENSSL_VERSION_NUMBER > 0x00907000L)
            if (method->it) {
               /*
                * New style ASN1
                * Decode ASN1 item in data
                */
               extstr = ASN1_item_d2i(NULL, &ext_value_data, X509_EXTENSION_get_data(ext)->length,
                                      ASN1_ITEM_ptr(method->it));
            } else {
               /*
                * Old style ASN1
                * Decode ASN1 item in data
                */
               extstr = method->d2i(NULL, &ext_value_data, X509_EXTENSION_get_data(ext)->length);
            }

#else
            extstr = method->d2i(NULL, &ext_value_data, ext->value->length);
#endif

            /*
             * Iterate through to find the dNSName field(s)
             */
            val = method->i2v(method, extstr, NULL);

            /*
             * dNSName shortname is "DNS"
             */
            for (j = 0; j < sk_CONF_VALUE_num(val); j++) {
               nval = sk_CONF_VALUE_value(val, j);
               if (bstrcmp(nval->name, "DNS")) {
                  if (bstrcasecmp(nval->value, host)) {
                     auth_success = true;
                     goto success;
                  }
               }
            }
         }
      }
   }

   /*
    * Try verifying against the subject name
    */
   if (!auth_success) {
      if ((subject = X509_get_subject_name(cert)) != NULL) {
         /*
          * Loop through all CNs
          */
         for (;;) {
            cnLastPos = X509_NAME_get_index_by_NID(subject, NID_commonName, cnLastPos);
            if (cnLastPos == -1) {
               break;
            }
            neCN = X509_NAME_get_entry(subject, cnLastPos);
            asn1CN = X509_NAME_ENTRY_get_data(neCN);
            if (bstrcasecmp((const char*)asn1CN->data, host)) {
               auth_success = true;
               break;
            }
         }
      }
   }

success:
   X509_free(cert);

   return auth_success;
}

/*
 * Create a new TLS_CONNECTION instance.
 *
 * Returns: Pointer to TLS_CONNECTION instance on success
 *          NULL on failure;
 */
TLS_CONNECTION *new_tls_connection(std::shared_ptr<TLS_Context> tls_ctx,
                                                   int fd,
                                                   bool server) {
   return new TLS_Connection(tls_ctx, fd, psk_client_cb, psk_server_cb);
//    return make_shared<TLS_Connection>(tls_ctx, fd, psk_client_cb, psk_server_cb);
}

/*
 * Free TLS_CONNECTION instance
 */
void free_tls_connection(TLS_CONNECTION *tls_conn)
{
   if (tls_conn != nullptr) {
      delete tls_conn;
   }
}

/* Does all the manual labor for tls_bsock_accept() and tls_bsock_connect() */
static inline bool openssl_bsock_session_start(BSOCK *bsock, bool server)
{
   TLS_CONNECTION *tls_conn = bsock->GetTlsConnection();
   int err;
   int flags;
   bool status = true;

   /* Ensure that socket is non-blocking */
   flags = bsock->set_nonblocking();

   /* start timer */
   bsock->timer_start = watchdog_time;
   bsock->clear_timed_out();
   bsock->set_killable(false);

   for (;;) {
      if (server) {
         err = SSL_accept(tls_conn->GetSsl());
      } else {
         err = SSL_connect(tls_conn->GetSsl());
      }

      /* Handle errors */
      switch (SSL_get_error(tls_conn->GetSsl(), err)) {
      case SSL_ERROR_NONE:
         status = true;
         goto cleanup;
      case SSL_ERROR_ZERO_RETURN:
         /* TLS connection was cleanly shut down */
         openssl_post_errors(bsock->get_jcr(), M_FATAL, _("Connect failure"));
         status = false;
         goto cleanup;
      case SSL_ERROR_WANT_READ:
         wait_for_readable_fd(bsock->m_fd, 10000, false);
         break;
      case SSL_ERROR_WANT_WRITE:
         wait_for_writable_fd(bsock->m_fd, 10000, false);
         break;
      default:
         /* Socket Error Occurred */
         openssl_post_errors(bsock->get_jcr(), M_FATAL, _("Connect failure"));
         status = false;
         goto cleanup;
      }

      if (bsock->is_timed_out()) {
         goto cleanup;
      }
   }

cleanup:
   /* Restore saved flags */
   bsock->restore_blocking(flags);
   /* Clear timer */
   bsock->timer_start = 0;
   bsock->set_killable(true);

   return status;
}

/*
 * Initiates a TLS connection with the server.
 *  Returns: true on success
 *           false on failure
 */
bool tls_bsock_connect(BSOCK *bsock)
{
   return openssl_bsock_session_start(bsock, false);
}

/*
 * Listens for a TLS connection from a client.
 *  Returns: true on success
 *           false on failure
 */
bool tls_bsock_accept(BSOCK *bsock)
{
   return openssl_bsock_session_start(bsock, true);
}

/*
 * Shutdown TLS_CONNECTION instance
 */
void tls_bsock_shutdown(BSOCK *bsock)
{
   /*
    * SSL_shutdown must be called twice to fully complete the process -
    * The first time to initiate the shutdown handshake, and the second to
    * receive the peer's reply.
    *
    * In addition, if the underlying socket is blocking, SSL_shutdown()
    * will not return until the current stage of the shutdown process has
    * completed or an error has occurred. By setting the socket blocking
    * we can avoid the ugly for()/switch()/select() loop.
    */
   int err;

   auto tls_conn = bsock->GetTlsConnection();

   btimer_t *tid;

   /* Set socket blocking for shutdown */
   bsock->set_blocking();

   tid = start_bsock_timer(bsock, 60 * 2);
   err = SSL_shutdown(tls_conn->GetSsl());
   stop_bsock_timer(tid);
   if (err == 0) {
      /* Complete shutdown */
      tid = start_bsock_timer(bsock, 60 * 2);
      err = SSL_shutdown(tls_conn->GetSsl());
      stop_bsock_timer(tid);
   }

   switch (SSL_get_error(tls_conn->GetSsl(), err)) {
   case SSL_ERROR_NONE:
      break;
   case SSL_ERROR_ZERO_RETURN:
      /* TLS connection was shut down on us via a TLS protocol-level closure */
      openssl_post_errors(bsock->get_jcr(), M_ERROR, _("TLS shutdown failure."));
      break;
   default:
      /* Socket Error Occurred */
      openssl_post_errors(bsock->get_jcr(), M_ERROR, _("TLS shutdown failure."));
      break;
   }
}

/* Does all the manual labor for tls_bsock_readn() and tls_bsock_writen() */
static inline int openssl_bsock_readwrite(BSOCK *bsock, char *ptr, int nbytes, bool write)
{
    TLS_CONNECTION *tls_conn = bsock->GetTlsConnection();
   int flags;
   int nleft = 0;
   int nwritten = 0;

   /* Ensure that socket is non-blocking */
   flags = bsock->set_nonblocking();

   /* start timer */
   bsock->timer_start = watchdog_time;
   bsock->clear_timed_out();
   bsock->set_killable(false);

   nleft = nbytes;

   while (nleft > 0) {
      if (write) {
         nwritten = SSL_write(tls_conn->GetSsl(), ptr, nleft);
      } else {
         nwritten = SSL_read(tls_conn->GetSsl(), ptr, nleft);
      }

      /* Handle errors */
      switch (SSL_get_error(tls_conn->GetSsl(), nwritten)) {
      case SSL_ERROR_NONE:
         nleft -= nwritten;
         if (nleft) {
            ptr += nwritten;
         }
         break;
      case SSL_ERROR_SYSCALL:
         if (nwritten == -1) {
            if (errno == EINTR) {
               continue;
            }
            if (errno == EAGAIN) {
               bmicrosleep(0, 20000); /* try again in 20 ms */
               continue;
            }
         }
         openssl_post_errors(bsock->get_jcr(), M_FATAL, _("TLS read/write failure."));
         goto cleanup;
      case SSL_ERROR_WANT_READ:
         wait_for_readable_fd(bsock->m_fd, 10000, false);
         break;
      case SSL_ERROR_WANT_WRITE:
         wait_for_writable_fd(bsock->m_fd, 10000, false);
         break;
      case SSL_ERROR_ZERO_RETURN:
         /* TLS connection was cleanly shut down */
         /* Fall through wanted */
      default:
         /* Socket Error Occured */
         openssl_post_errors(bsock->get_jcr(), M_FATAL, _("TLS read/write failure."));
         goto cleanup;
      }

      /* Everything done? */
      if (nleft == 0) {
         goto cleanup;
      }

      /* Timeout/Termination, let's take what we can get */
      if (bsock->is_timed_out() || bsock->is_terminated()) {
         goto cleanup;
      }
   }

cleanup:
   /* Restore saved flags */
   bsock->restore_blocking(flags);

   /* Clear timer */
   bsock->timer_start = 0;
   bsock->set_killable(true);

   return nbytes - nleft;
}

int tls_bsock_writen(BSOCK *bsock, char *ptr, int32_t nbytes)
{
   return openssl_bsock_readwrite(bsock, ptr, nbytes, true);
}

int tls_bsock_readn(BSOCK *bsock, char *ptr, int32_t nbytes)
{
   return openssl_bsock_readwrite(bsock, ptr, nbytes, false);
}
#endif /* HAVE_TLS  && HAVE_OPENSSL */
