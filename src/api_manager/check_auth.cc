/*
 * Copyright (C) Endpoints Server Proxy Authors
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "src/api_manager/check_auth.h"

#include <chrono>
#include <string>

#include "include/api_manager/api_manager.h"
#include "include/api_manager/auth.h"
#include "include/api_manager/request.h"
#include "src/api_manager/auth/lib/auth_jwt_validator.h"
#include "src/api_manager/auth/lib/json_util.h"

using ::google::api_manager::auth::Certs;
using ::google::api_manager::auth::JwtCache;
using ::google::api_manager::auth::JwtValue;
using ::google::api_manager::auth::GetStringValue;
using ::google::api_manager::auth::JwtValidator;
using ::google::api_manager::utils::Status;
using ::google::protobuf::util::error::Code;
using std::chrono::system_clock;

namespace google {
namespace api_manager {

namespace {
const char kAccessTokenName[] = "access_token";
const char kAuthHeader[] = "authorization";
const char kBearer[] = "Bearer ";
// The lifetime of a public key cache entry. Unit: seconds.
const int kPubKeyCacheDuration = 300;

// An AuthChecker object is created for every incoming request. It authenticates
// the request, extracts user info from the auth token and sets it to the
// request context.
class AuthChecker : public std::enable_shared_from_this<AuthChecker> {
 public:
  AuthChecker(std::shared_ptr<context::RequestContext> context,
              std::function<void(Status status)> continuation);

  // Check auth for a given request. This is the starting point to enter
  // the auth state machine.
  void Check();

 private:
  /*** Steps in auth state machine, ordered in execution sequence. ***/

  // Not all the steps are executed for every request.
  // For example, in case of a JWT cache hit, only four steps are executed:
  // GetAuthToken() --> LookupJwtCache() --> CheckAudience() --> PassUserInfo()
  // In the case of a JWT cache miss, but a key cache hit, the steps are:
  // GetAuthToken() --> LookupJwtCache() --> ParseJwt() --> CheckAudience() -->
  // InitKey() --> VerifySignature() --> PassUserInfo()
  void GetAuthToken();

  void LookupJwtCache();

  void ParseJwt();

  void CheckAudience(bool cache_hit);

  void InitKey();

  void DiscoverJwksUri(const std::string &url);

  // Callback function for open ID discovery http fetch.
  void PostFetchJwksUri(Status status, std::string &&body);

  void FetchPubKey(const std::string &url);

  // Callback function for public key http fetch.
  void PostFetchPubKey(Status status, std::string &&body);

  void VerifySignature();

  void PassUserInfoOnSuccess();

  /*** Helper functions ***/

  // Returns a shared pointer of this AuthChecker object.
  std::shared_ptr<AuthChecker> GetPtr() { return shared_from_this(); }

  // Helper function to send a http GET request.
  Status HttpFetch(const std::string &url,
                   std::function<void(Status, std::string &&)> continuation);

  // Sets state_ to fail and sets error code and message when authentication
  // fails.
  void Unauthorized(const std::string &error);

  /*** Member Variables. ***/

  // Request context.
  std::shared_ptr<context::RequestContext> context_;

  // JWT validator.
  std::unique_ptr<auth::JwtValidator> validator_;

  // User info extracted from auth token.
  UserInfo user_info_;

  // Pointer to access ESP running environment.
  ApiManagerEnvInterface *env_;

  // auth token.
  std::string auth_token_;

  // The final continuation function.
  std::function<void(Status status)> on_done_;
};

AuthChecker::AuthChecker(std::shared_ptr<context::RequestContext> context,
                         std::function<void(Status status)> continuation)
    : context_(context),
      env_(context_->service_context()->env()),
      on_done_(continuation) {}

void AuthChecker::Check() {
  if (!context_->service_context()->RequireAuth() ||
      context_->method() == nullptr || !context_->method()->auth()) {
    env_->LogDebug("Auth not required.");
    on_done_(Status::OK);
    return;
  }

  GetAuthToken();
  if (auth_token_.empty()) {
    Unauthorized("Missing or invalid credentials");
    return;
  }
  context_->request()->SetAuthToken(auth_token_);

  env_->LogDebug(std::string("auth token: ") + auth_token_);

  LookupJwtCache();
}

void AuthChecker::GetAuthToken() {
  Request *r = context_->request();
  std::string auth_header;
  if (!r->FindHeader(kAuthHeader, &auth_header)) {
    // When authorization header is missing, check query parameter.
    r->FindQuery(kAccessTokenName, &auth_token_);
    return;
  }

  static const size_t bearer_len = sizeof(kBearer) - 1;
  if (auth_header.size() <= bearer_len ||
      auth_header.compare(0, bearer_len, kBearer) != 0) {
    // Authorization header is not long enough, or authorization header does
    // not begin with "Bearer ", set auth_token_ to empty string.
    auth_token_ = std::string();
    return;
  }

  auth_token_ = auth_header.substr(bearer_len);
}

void AuthChecker::LookupJwtCache() {
  bool remove = false;  // whether or not need to remove an expired entry.
  bool cache_hit = false;
  JwtCache &jwt_cache = context_->service_context()->jwt_cache();
  {
    JwtCache::ScopedLookup lookup(&jwt_cache, auth_token_);
    if (lookup.Found()) {
      JwtValue *val = lookup.value();
      if (system_clock::now() <= val->exp) {
        // Cache hit and cache entry is not expired.
        user_info_ = val->user_info;
        cache_hit = true;
      } else {
        // Need to removes the expired cache entry.
        remove = true;
      }
    }
  }
  if (remove) {
    jwt_cache.Remove(auth_token_);
  }

  if (cache_hit) {
    CheckAudience(true);
  } else {
    ParseJwt();
  }
}

void AuthChecker::ParseJwt() {
  if (validator_ == nullptr) {
    validator_ = JwtValidator::Create(auth_token_.c_str(), auth_token_.size());
    if (validator_ == nullptr) {
      Unauthorized("Internal error");
      return;
    }
  }

  Status status = validator_->Parse(&user_info_);
  if (!status.ok()) {
    Unauthorized(status.message());
    return;
  }

  CheckAudience(false);
}

void AuthChecker::CheckAudience(bool cache_hit) {
  std::string audience = user_info_.audiences.empty()
                             ? std::string()
                             : user_info_.AudiencesAsString();
  context_->set_auth_issuer(user_info_.issuer);
  context_->set_auth_audience(audience);

  if (!context_->method()->isIssuerAllowed(user_info_.issuer)) {
    Unauthorized("Issuer not allowed");
    return;
  }

  // The audience from the JWT must
  //   - Equals to service_name or
  //   - Explicitly allowed by the issuer in the method configuration.
  // Otherwise the JWT is rejected.
  const std::string &service_name = context_->service_context()->service_name();
  if (user_info_.audiences.find(service_name) == user_info_.audiences.end() &&
      !context_->method()->isAudienceAllowed(user_info_.issuer,
                                             user_info_.audiences)) {
    Unauthorized("Audience not allowed");
    return;
  }
  if (cache_hit) {
    PassUserInfoOnSuccess();
  } else {
    InitKey();
  }
}

void AuthChecker::InitKey() {
  Certs &key_cache = context_->service_context()->certs();
  auto cert = key_cache.GetCert(user_info_.issuer);

  if (cert == nullptr || system_clock::now() > cert->second) {
    // Key has not been fetched or has expired.
    std::string url;
    bool tryOpenId =
        context_->service_context()->GetJwksUri(user_info_.issuer, &url);
    if (url.empty()) {
      Unauthorized("Cannot determine the URI of the key");
      return;
    }

    if (tryOpenId) {
      DiscoverJwksUri(url);
    } else {
      // JwksUri is available. No need to try openID discovery.
      FetchPubKey(url);
    }
  } else {
    // Key is in the cache, next step is to verify signature.
    VerifySignature();
  }
}

void AuthChecker::DiscoverJwksUri(const std::string &url) {
  auto pChecker = GetPtr();
  Status status = HttpFetch(url, [pChecker](Status status, std::string &&body) {
    pChecker->PostFetchJwksUri(status, std::move(body));
  });
  if (!status.ok()) {
    Unauthorized("Unable to fetch URI of the key via OpenID discovery");
    return;
  }
}

void AuthChecker::PostFetchJwksUri(Status status, std::string &&body) {
  if (!status.ok()) {
    context_->service_context()->SetJwksUri(user_info_.issuer, std::string(),
                                            false);
    Unauthorized("Unable to fetch URI of the key via OpenID discovery");
    return;
  }

  // Parse discovery doc and extract jwks_uri
  grpc_json *discovery_json = grpc_json_parse_string_with_len(
      const_cast<char *>(body.c_str()), body.size());
  const char *jwks_uri;
  if (discovery_json != nullptr) {
    jwks_uri = GetStringValue(discovery_json, "jwks_uri");
    grpc_json_destroy(discovery_json);
  } else {
    jwks_uri = nullptr;
  }

  if (jwks_uri == nullptr) {
    env_->LogError("OpenID discovery failed due to invalid doc format");
    context_->service_context()->SetJwksUri(user_info_.issuer, std::string(),
                                            false);
    Unauthorized("Unable to fetch URI of the key via OpenID discovery");
    return;
  }

  // OpenID discovery completed. Set jwks_uri for the issuer in cache.
  context_->service_context()->SetJwksUri(user_info_.issuer, jwks_uri, false);

  FetchPubKey(jwks_uri);
}

void AuthChecker::FetchPubKey(const std::string &url) {
  auto pChecker = GetPtr();
  Status status = HttpFetch(url, [pChecker](Status status, std::string &&body) {
    pChecker->PostFetchPubKey(status, std::move(body));
  });
  if (!status.ok()) {
    Unauthorized("Unable to fetch public key");
    return;
  }
}

void AuthChecker::PostFetchPubKey(Status status, std::string &&body) {
  if (!status.ok() || body.empty()) {
    Unauthorized("Unable to fetch verification key");
    return;
  }

  Certs &key_cache = context_->service_context()->certs();
  key_cache.Update(
      user_info_.issuer, std::move(body),
      system_clock::now() + std::chrono::seconds(kPubKeyCacheDuration));
  VerifySignature();
}

void AuthChecker::VerifySignature() {
  Certs &key_cache = context_->service_context()->certs();
  auto cert = key_cache.GetCert(user_info_.issuer);
  if (cert == nullptr) {
    Unauthorized("Missing verification key");
    return;
  }

  Status status =
      validator_->VerifySignature(cert->first.c_str(), cert->first.size());
  if (!status.ok()) {
    Unauthorized(status.message());
    return;
  }

  // Inserts the entry to JwtCache.
  JwtCache &cache = context_->service_context()->jwt_cache();
  cache.Insert(auth_token_, user_info_, validator_->GetExpirationTime(),
               system_clock::now());

  PassUserInfoOnSuccess();
}

void AuthChecker::PassUserInfoOnSuccess() {
  context_->request()->SetUserInfo(user_info_);
  on_done_(Status::OK);
}

void AuthChecker::Unauthorized(const std::string &error) {
  on_done_(Status(Code::UNAUTHENTICATED,
                  std::string("JWT validation failed: ") + error,
                  Status::AUTH));
}

Status AuthChecker::HttpFetch(
    const std::string &url,
    std::function<void(Status, std::string &&)> continuation) {
  env_->LogDebug(std::string("http fetch: ") + url);

  std::unique_ptr<HTTPRequest> request(
      new HTTPRequest([continuation](Status status, std::string &&body) {
        status.SetErrorCause(Status::AUTH);
        continuation(status, std::move(body));
      }));
  if (!request) {
    return Status(Code::UNAUTHENTICATED, "Out of memory", Status::INTERNAL);
  }

  request->set_method("GET").set_url(url);

  return env_->RunHTTPRequest(std::move(request));
}

}  // namespace

void CheckAuth(std::shared_ptr<context::RequestContext> context,
               std::function<void(Status status)> continuation) {
  std::shared_ptr<AuthChecker> authChecker =
      std::make_shared<AuthChecker>(context, continuation);
  authChecker->Check();
}

}  // namespace api_manager
}  // namespace google