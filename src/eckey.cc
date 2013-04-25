#include <string.h>

#include <v8.h>

#include <node.h>
#include <node_buffer.h>
#include <node_internals.h>

#include <openssl/ecdsa.h>
#include <openssl/evp.h>

#include "common.h"
#include "eckey.h"

using namespace std;
using namespace v8;
using namespace node;

int static inline EC_KEY_regenerate_key(EC_KEY *eckey, const BIGNUM *priv_key)
{
  int ok = 0;
  BN_CTX *ctx = NULL;
  EC_POINT *pub_key = NULL;

  if (!eckey) return 0;

  const EC_GROUP *group = EC_KEY_get0_group(eckey);

  if ((ctx = BN_CTX_new()) == NULL)
    goto err;

  pub_key = EC_POINT_new(group);

  if (pub_key == NULL)
    goto err;

  if (!EC_POINT_mul(group, pub_key, priv_key, NULL, NULL, ctx))
    goto err;

  EC_KEY_set_private_key(eckey,priv_key);
  EC_KEY_set_public_key(eckey,pub_key);

  ok = 1;

 err:

  if (pub_key)
    EC_POINT_free(pub_key);
  if (ctx != NULL)
    BN_CTX_free(ctx);

  return(ok);
}

void BitcoinKey::Generate()
{
  if (!EC_KEY_generate_key(ec)) {
    lastError = "Error from EC_KEY_generate_key";
    return;
  }

  hasPublic = true;
  hasPrivate = true;
}

int BitcoinKey::VerifySignature(const unsigned char *digest, int digest_len,
                    const unsigned char *sig, int sig_len)
{
  return ECDSA_verify(0, digest, digest_len, sig, sig_len, ec);
}

void BitcoinKey::EIO_VerifySignature(uv_work_t *req)
{
  verify_sig_baton_t *b = static_cast<verify_sig_baton_t *>(req->data);

  b->result = b->key->VerifySignature(
    b->digest, b->digestLen,
    b->sig, b->sigLen
  );
}

ECDSA_SIG *BitcoinKey::Sign(const unsigned char *digest, int digest_len)
{
  ECDSA_SIG *sig;

  sig = ECDSA_do_sign(digest, digest_len, ec);
  if (sig == NULL) {
    // TODO: ERROR
  }

  return sig;
}

void BitcoinKey::Init(Handle<Object> target)
{
  HandleScope scope;
  Local<FunctionTemplate> t = FunctionTemplate::New(New);

  s_ct = Persistent<FunctionTemplate>::New(t);
  s_ct->InstanceTemplate()->SetInternalFieldCount(1);
  s_ct->SetClassName(String::NewSymbol("BitcoinKey"));

  // Accessors
  s_ct->InstanceTemplate()->SetAccessor(String::New("private"),
                                        GetPrivate, SetPrivate);
  s_ct->InstanceTemplate()->SetAccessor(String::New("public"),
                                        GetPublic, SetPublic);

  // Methods
  NODE_SET_PROTOTYPE_METHOD(s_ct, "verifySignature", VerifySignature);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "verifySignatureSync", VerifySignatureSync);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "regenerateSync", RegenerateSync);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "toDER", ToDER);
  NODE_SET_PROTOTYPE_METHOD(s_ct, "signSync", SignSync);

  // Static methods
  NODE_SET_METHOD(s_ct->GetFunction(), "generateSync", GenerateSync);
  NODE_SET_METHOD(s_ct->GetFunction(), "fromDER", FromDER);

  target->Set(String::NewSymbol("BitcoinKey"),
              s_ct->GetFunction());
}

BitcoinKey::BitcoinKey() :
  lastError(NULL),
  hasPrivate(false),
  hasPublic(false)
{
  ec = EC_KEY_new_by_curve_name(NID_secp256k1);
  if (ec == NULL) {
    lastError = "Error from EC_KEY_new_by_curve_name";
  }
}

BitcoinKey::~BitcoinKey()
{
  EC_KEY_free(ec);
}

BitcoinKey*
BitcoinKey::New()
{
  HandleScope scope;

  Local<Object> k = s_ct->GetFunction()->NewInstance(0, NULL);
  if (k.IsEmpty()) return NULL;

  return ObjectWrap::Unwrap<BitcoinKey>(k);
}

Handle<Value>
BitcoinKey::New(const Arguments& args)
{
  if (!args.IsConstructCall()) {
    return FromConstructorTemplate(s_ct, args);
  }

  HandleScope scope;

  BitcoinKey* key = new BitcoinKey();
  if (key->lastError != NULL) {
    return VException(key->lastError);
  }

  key->Wrap(args.Holder());

  return scope.Close(args.This());
}

Handle<Value>
BitcoinKey::GenerateSync(const Arguments& args)
{
  HandleScope scope;

  BitcoinKey* key = BitcoinKey::New();

  key->Generate();

  if (key->lastError != NULL) {
    return VException(key->lastError);
  }

  return scope.Close(key->handle_);
}

Handle<Value>
BitcoinKey::GetPrivate(Local<String> property, const AccessorInfo& info)
{
  HandleScope scope;
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(info.Holder());

  if (!key->hasPrivate) {
    return scope.Close(Null());
  }

  const BIGNUM *bn = EC_KEY_get0_private_key(key->ec);
  int priv_size = BN_num_bytes(bn);

  if (bn == NULL) {
    // TODO: ERROR: "Error from EC_KEY_get0_private_key(pkey)"
    return scope.Close(Null());
  }

  if (priv_size > 32) {
    // TODO: ERROR: "Secret too large (Incorrect curve parameters?)"
    return scope.Close(Null());
  }

  unsigned char *priv = (unsigned char *)malloc(32);

  int n = BN_bn2bin(bn, &priv[32 - priv_size]);

  if (n != priv_size) {
    // TODO: ERROR: "Error from BN_bn2bin(bn, &priv[32 - priv_size])"
    return scope.Close(Null());
  }

  Buffer *priv_buf = Buffer::New(32);
  memcpy(Buffer::Data(priv_buf), priv, 32);

  free(priv);

  return scope.Close(priv_buf->handle_);
}

void
BitcoinKey::SetPrivate(Local<String> property, Local<Value> value, const AccessorInfo& info)
{
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(info.Holder());
  Handle<Object> buffer = value->ToObject();
  const unsigned char *data = (const unsigned char*) Buffer::Data(buffer);

  BIGNUM *bn = BN_bin2bn(data,Buffer::Length(buffer),BN_new());
  EC_KEY_set_private_key(key->ec, bn);
  BN_clear_free(bn);

  key->hasPrivate = true;
}

Handle<Value>
BitcoinKey::GetPublic(Local<String> property, const AccessorInfo& info)
{
  HandleScope scope;
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(info.Holder());

  if (!key->hasPublic) {
    return scope.Close(Null());
  }

  // Export public
  int pub_size = i2o_ECPublicKey(key->ec, NULL);
  if (!pub_size) {
    // TODO: ERROR: "Error from i2o_ECPublicKey(key->ec, NULL)"
    return scope.Close(Null());
  }
  unsigned char *pub_begin, *pub_end;
  pub_begin = pub_end = (unsigned char *)malloc(pub_size);

  if (i2o_ECPublicKey(key->ec, &pub_end) != pub_size) {
    // TODO: ERROR: "Error from i2o_ECPublicKey(key->ec, &pub)"
    return scope.Close(Null());
  }
  Buffer *pub_buf = Buffer::New(pub_size);
  memcpy(Buffer::Data(pub_buf), pub_begin, pub_size);

  free(pub_begin);

  return scope.Close(pub_buf->handle_);
}

void
BitcoinKey::SetPublic(Local<String> property, Local<Value> value, const AccessorInfo& info)
{
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(info.Holder());
  Handle<Object> buffer = value->ToObject();
  const unsigned char *data = (const unsigned char*) Buffer::Data(buffer);

  if (!o2i_ECPublicKey(&(key->ec), &data, Buffer::Length(buffer))) {
    // TODO: Error
    return;
  }

  key->hasPublic = true;
}

Handle<Value>
BitcoinKey::RegenerateSync(const Arguments& args)
{
  HandleScope scope;
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(args.This());

  if (!key->hasPrivate) {
    return VException("Regeneration requires a private key.");
  }

  EC_KEY *old = key->ec;

  key->ec = EC_KEY_new_by_curve_name(NID_secp256k1);
  if (EC_KEY_regenerate_key(key->ec, EC_KEY_get0_private_key(old)) == 1) {
    key->hasPublic = true;
  }

  EC_KEY_free(old);

  return scope.Close(Undefined());
}

Handle<Value>
BitcoinKey::ToDER(const Arguments& args)
{
  HandleScope scope;
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(args.This());

  if (!key->hasPrivate || !key->hasPublic) {
    return scope.Close(Null());
  }

  // Export DER
  int der_size = i2d_ECPrivateKey(key->ec, NULL);
  if (!der_size) {
    // TODO: ERROR: "Error from i2d_ECPrivateKey(key->ec, NULL)"
    return scope.Close(Null());
  }
  unsigned char *der_begin, *der_end;
  der_begin = der_end = (unsigned char *)malloc(der_size);

  if (i2d_ECPrivateKey(key->ec, &der_end) != der_size) {
    // TODO: ERROR: "Error from i2d_ECPrivateKey(key->ec, &der_end)"
    return scope.Close(Null());
  }
  Buffer *der_buf = Buffer::New(der_size);
  memcpy(Buffer::Data(der_buf), der_begin, der_size);

  free(der_begin);

  return scope.Close(der_buf->handle_);
}

Handle<Value>
BitcoinKey::FromDER(const Arguments& args)
{
  HandleScope scope;

  if (args.Length() != 1) {
    return VException("One argument expected: der");
  }
  if (!Buffer::HasInstance(args[0])) {
    return VException("Argument 'der' must be of type Buffer");
  }

  BitcoinKey* key = new BitcoinKey();
  if (key->lastError != NULL) {
    return VException(key->lastError);
  }

  Handle<Object> der_buf = args[0]->ToObject();
  const unsigned char *data = (const unsigned char*) Buffer::Data(der_buf);

  if (!d2i_ECPrivateKey(&(key->ec), &data, Buffer::Length(der_buf))) {
    return VException("Error from d2i_ECPrivateKey(&key, &data, len)");
  }

  key->hasPrivate = true;
  key->hasPublic = true;

  Handle<Function> cons = s_ct->GetFunction();
  Handle<Value> external = External::New(key);
  Handle<Value> result = cons->NewInstance(1, &external);

  return scope.Close(result);
}

Handle<Value>
BitcoinKey::VerifySignature(const Arguments& args)
{
  HandleScope scope;
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(args.This());

  if (args.Length() != 3) {
    return VException("Three arguments expected: hash, sig, callback");
  }
  if (!Buffer::HasInstance(args[0])) {
    return VException("Argument 'hash' must be of type Buffer");
  }
  if (!Buffer::HasInstance(args[1])) {
    return VException("Argument 'sig' must be of type Buffer");
  }
  REQ_FUN_ARG(2, cb);
  if (!key->hasPublic) {
    return VException("BitcoinKey does not have a public key set");
  }

  Handle<Object> hash_buf = args[0]->ToObject();
  Handle<Object> sig_buf = args[1]->ToObject();

  if (Buffer::Length(hash_buf) != 32) {
    return VException("Argument 'hash' must be Buffer of length 32 bytes");
  }

  verify_sig_baton_t *baton = new verify_sig_baton_t();
  baton->key = key;
  baton->digest = (unsigned char *)Buffer::Data(hash_buf);
  baton->digestLen = Buffer::Length(hash_buf);
  baton->digestBuf = Persistent<Object>::New(hash_buf);
  baton->sig = (unsigned char *)Buffer::Data(sig_buf);
  baton->sigLen = Buffer::Length(sig_buf);
  baton->sigBuf = Persistent<Object>::New(sig_buf);
  baton->result = -1;
  baton->cb = Persistent<Function>::New(cb);

  key->Ref();

  uv_work_t *req = new uv_work_t;
  req->data = baton;

  uv_queue_work(uv_default_loop(), req, EIO_VerifySignature, (uv_after_work_cb) VerifySignatureCallback);

  return scope.Close(Undefined());
}

void
BitcoinKey::VerifySignatureCallback(uv_work_t *req, int status)
{
  HandleScope scope;
  verify_sig_baton_t *baton = static_cast<verify_sig_baton_t *>(req->data);

  baton->key->Unref();
  baton->digestBuf.Dispose();
  baton->sigBuf.Dispose();

  Local<Value> argv[2];

  argv[0] = Local<Value>::New(Null());
  argv[1] = Local<Value>::New(Null());
  if (baton->result == -1) {
    argv[0] = Exception::TypeError(String::New("Error during ECDSA_verify"));
  } else if (baton->result == 0) {
    // Signature invalid
    argv[1] = Local<Value>::New(Boolean::New(false));
  } else if (baton->result == 1) {
    // Signature valid
    argv[1] = Local<Value>::New(Boolean::New(true));
  } else {
    argv[0] = Exception::TypeError(
      String::New("ECDSA_verify gave undefined return value"));
  }

  TryCatch try_catch;

  baton->cb->Call(Context::GetCurrent()->Global(), 2, argv);


  baton->cb.Dispose();

  delete baton;
  delete req;

  if (try_catch.HasCaught()) {
    FatalException(try_catch);
  }
}

Handle<Value>
BitcoinKey::VerifySignatureSync(const Arguments& args)
{
  HandleScope scope;
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(args.This());

  if (args.Length() != 2) {
    return VException("Two arguments expected: hash, sig");
  }
  if (!Buffer::HasInstance(args[0])) {
    return VException("Argument 'hash' must be of type Buffer");
  }
  if (!Buffer::HasInstance(args[1])) {
    return VException("Argument 'sig' must be of type Buffer");
  }
  if (!key->hasPublic) {
    return VException("BitcoinKey does not have a public key set");
  }

  Handle<Object> hash_buf = args[0]->ToObject();
  Handle<Object> sig_buf = args[1]->ToObject();

  const unsigned char *hash_data = (unsigned char *) Buffer::Data(hash_buf);
  const unsigned char *sig_data = (unsigned char *) Buffer::Data(sig_buf);

  unsigned int hash_len = Buffer::Length(hash_buf);
  unsigned int sig_len = Buffer::Length(sig_buf);

  if (hash_len != 32) {
    return VException("Argument 'hash' must be Buffer of length 32 bytes");
  }

  // Verify signature
  int result = key->VerifySignature(hash_data, hash_len, sig_data, sig_len);

  if (result == -1) {
    return VException("Error during ECDSA_verify");
  } else if (result == 0) {
    // Signature invalid
    return scope.Close(Boolean::New(false));
  } else if (result == 1) {
    // Signature valid
    return scope.Close(Boolean::New(true));
  } else {
    return VException("ECDSA_verify gave undefined return value");
  }
}

Handle<Value>
BitcoinKey::SignSync(const Arguments& args)
{
  HandleScope scope;
  BitcoinKey* key = node::ObjectWrap::Unwrap<BitcoinKey>(args.This());

  if (args.Length() != 1) {
    return VException("One argument expected: hash");
  }
  if (!Buffer::HasInstance(args[0])) {
    return VException("Argument 'hash' must be of type Buffer");
  }
  if (!key->hasPrivate) {
    return VException("BitcoinKey does not have a private key set");
  }

  Handle<Object> hash_buf = args[0]->ToObject();

  const unsigned char *hash_data = (unsigned char *) Buffer::Data(hash_buf);

  unsigned int hash_len = Buffer::Length(hash_buf);

  if (hash_len != 32) {
    return VException("Argument 'hash' must be Buffer of length 32 bytes");
  }

  // Create signature
  ECDSA_SIG *sig = key->Sign(hash_data, hash_len);

  // Export DER
  int der_size = i2d_ECDSA_SIG(sig, NULL);
  if (!der_size) {
    // TODO: ERROR: "Error from i2d_ECPrivateKey(key->ec, NULL)"
    return scope.Close(Null());
  }
  unsigned char *der_begin, *der_end;
  der_begin = der_end = (unsigned char *)malloc(der_size);

  if (i2d_ECDSA_SIG(sig, &der_end) != der_size) {
    // TODO: ERROR: "Error from i2d_ECPrivateKey(key->ec, &der_end)"
    return scope.Close(Null());
  }
  Buffer *der_buf = Buffer::New(der_size);
  memcpy(Buffer::Data(der_buf), der_begin, der_size);

  free(der_begin);
  ECDSA_SIG_free(sig);

  return scope.Close(der_buf->handle_);
}

Persistent<FunctionTemplate> BitcoinKey::s_ct;
