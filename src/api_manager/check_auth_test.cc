// Copyright (C) Endpoints Server Proxy Authors
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.
//
////////////////////////////////////////////////////////////////////////////////
//
#include "src/api_manager/check_auth.h"

#include "src/api_manager/check_workflow.h"
#include "src/api_manager/context/service_context.h"
#include "src/api_manager/mock_api_manager_environment.h"
#include "src/api_manager/mock_request.h"

using ::testing::_;
using ::testing::AllOf;
using ::testing::DoAll;
using ::testing::Field;
using ::testing::Invoke;
using ::testing::Mock;
using ::testing::Return;

using ::google::api_manager::utils::Status;
using ::google::protobuf::util::error::Code;

namespace google {
namespace api_manager {

namespace {

const char kAccessTokenName[] = "access_token";
const char kAuthHeader[] = "authorization";
const char kBearer[] = "Bearer ";

const char kServiceConfig[] =
    "name: \"endpoints-test.cloudendpointsapis.com\"\n"
    "authentication {\n"
    "    providers: [\n"
    "    {\n"
    "      id: \"issuer1\"\n"
    "      issuer: \"https://issuer1.com\"\n"
    "    },\n"
    "    {\n"
    "      id: \"openid_fail\"\n"
    "      issuer: \"http://openid_fail\"\n"
    "    },\n"
    "    {\n"
    "      id: \"issuer2\"\n"
    "      issuer: \"https://issuer2.com\"\n"
    "      jwks_uri: \"https://issuer2.com/pubkey\"\n"
    "    }\n"
    "    ],\n"
    "    rules: {\n"
    "      selector: \"ListShelves\"\n"
    "      requirements: [\n"
    "      {\n"
    "        provider_id: \"issuer1\"\n"
    "      },\n"
    "      {\n"
    "        provider_id: \"openid_fail\"\n"
    "      },\n"
    "      {\n"
    "        provider_id: \"issuer2\"\n"
    "      }\n"
    "      ]\n"
    "    }\n"
    "}\n"
    "http {\n"
    "  rules {\n"
    "    selector: \"ListShelves\"\n"
    "    get: \"/ListShelves\"\n"
    "  }\n"
    "}\n"
    "control {\n"
    "  environment : \"http://127.0.0.1:8081\"\n"
    "}\n";

const char kSub[] = "end-user-id";
const char kSub2[] = "another-user-id";
const char kIss[] = "https://issuer1.com";
const char kIss2[] = "https://issuer2.com";

// Auth token generated with the following header and payload.
//{
// "alg": "RS256",
// "typ": "JWT",
// "kid": "b3319a147514df7ee5e4bcdee51350cc890cc89e"
//}
//{
// "iss": "https://issuer1.com",
// "sub": "end-user-id",
// "aud": "endpoints-test.cloudendpointsapis.com",
// "iat": 1461779321,
// "exp": 2461782921
//}
const char kToken[] =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImIzMzE5YTE0NzUxNGRmN2VlNWU0Ym"
    "NkZWU1MTM1MGNjODkwY2M4OWUifQ."
    "eyJpc3MiOiJodHRwczovL2lzc3VlcjEuY29tIiwic3ViIjoiZW5kLXVzZXItaWQiLCJhdWQiOi"
    "JlbmRwb2ludHMtdGVzdC5jbG91ZGVuZHBvaW50c2FwaXMuY29tIiwiaWF0IjoxNDYxNzc5MzIx"
    "LCJleHAiOjI0NjE3ODI5MjF9.iiJj93x_KNSIh14nthz_N_"
    "JsA6ZAZfQQxECrmhalrcw5jnlKKIwI_QFnv8y9EFyIHLfEt56-"
    "GUXH7uLhed1sTJUNTd8sXyuFdSK1Cd3jAozvvZaYazhNIYC9ljh7hhm-"
    "rHkirWTu1GxdRaJD3Az-B-VX3C_OWY9oHR2mhw0zxkEcMAgjf7GuGWr-AYtDmAMD_"
    "fE8o7oXvD9eg506E5mcDa208m0N8Ysc3Ibdmfnux5B1pPB-5M-O2u2lVLjhne7SMCG9wv-"
    "nnXCy9iAcTgCt6VmBtehZOBDQ0q8_08aWGoWBXntLwdXinVIs-zR-"
    "4YUumdkEIIPF3IYE6ZAlloWG7Q";

// kToken2 is the same as kToken except that "sub" is "another-user-id".
const char kToken2[] =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImIzMzE5YTE0NzUxNGRmN2VlNWU0Ym"
    "NkZWU1MTM1MGNjODkwY2M4OWUifQ.eyJpc3MiOiJodHRwczovL2lzc3VlcjEuY29tIiwic3ViI"
    "joiYW5vdGhlci11c2VyLWlkIiwiYXVkIjoiZW5kcG9pbnRzLXRlc3QuY2xvdWRlbmRwb2ludHN"
    "hcGlzLmNvbSIsImlhdCI6MTQ2MTc3OTMyMSwiZXhwIjoyNDYxNzgyOTIxfQ.ASIXY1N3fmGuDY"
    "8-Xg6lCxiXM51wtTiGRmcYMk6_q_91D3D9cswVMfNp7YLf6sA4KQFxoTFAiWRqxT-kPzF5o2O8"
    "ga4CY0VZAsiXRm-YTe7O8T2kFrVJOkABQIyNZgln8Sm15bO1MSyClj8Ti2qRAYPeoDM57X0f-u"
    "0nQsWv0X8BsLvlJfu1J-4n08l_eEUFZlYGwpWCKfwvklmWXYfXdcJMeQ_poVqOti8dvSnqVi_Z"
    "0Yu1r5Xuq45q4WNqQ9PRk1HFsWB7uV25m8fU2VpbRLFka6F9MZu4dU9gZoGCGDTtauCHiMqBTv"
    "6vON8GbcB7w8pEhS1hK6FOehe4qZKnSA";

// kTokenOpenIdFail is the same as kToken except that "iss" is
// "http://openid_fail".
const char kTokenOpenIdFail[] =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImIzMzE5YTE0NzUxNGRmN2VlNWU0Ym"
    "NkZWU1MTM1MGNjODkwY2M4OWUifQ."
    "eyJpc3MiOiJodHRwOi8vb3BlbmlkX2ZhaWwiLCJzdWIiOiJlbmQtdXNlci1pZCIsImF1ZCI6Im"
    "VuZHBvaW50cy10ZXN0LmNsb3VkZW5kcG9pbnRzYXBpcy5jb20iLCJpYXQiOjE0NjE3NzkzMjEs"
    "ImV4cCI6MjQ2MTc4MjkyMX0.kauli_A55XB3AI-ZHrzlZs87VlMF_"
    "iRyxAJs0BOCIZ0pYrsH4gKpheepafoJfYJcP5HYeC4JZuivKmfZn8hVjHZ-crXhxvnQf0AM-"
    "nI4S80tuWewcvKQq3tpyoyjw0DAu4sI61ejCINvc2qEpiyp4jBcww1xxOFXbCOvSTbfJzISGSe"
    "Kmqs5ryGHFyW-"
    "rsGau030xa4ZnJo4qjzEaFqf9UwbWoEGhmJLHx6AWJUPnMtHN1YGZkCO7OXBk7gOOlVd5iNR-"
    "OHDbpUEEYI2KM5N2MNdjN5QaAIwyvDnWTA3ivetbiiNP2sjt9Ar3fTkfO_"
    "bjHTvoHiUKvPLTJfWeLVSzQ";

// kTokenOpenIdFail2 is the same as kTokenOpenIdFail except that "sub" is
// "another-user-id".
const char kTokenOpenIdFail2[] =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImIzMzE5YTE0NzUxNGRmN2VlNWU0Ym"
    "NkZWU1MTM1MGNjODkwY2M4OWUifQ."
    "eyJpc3MiOiJodHRwOi8vb3BlbmlkX2ZhaWwiLCJzdWIiOiJhbm90aGVyLXVzZXItaWQiLCJhdW"
    "QiOiJlbmRwb2ludHMtdGVzdC5jbG91ZGVuZHBvaW50c2FwaXMuY29tIiwiaWF0IjoxNDYxNzc5"
    "MzIxLCJleHAiOjI0NjE3ODI5MjF9.DYsI1A0CDZUbIniGAQy0JXyE9KjhmbSMxNzIrm_"
    "5ogvwAXgpx4vStYtGU6w_Lv1vAtQHONY6zK4qzNBi3h-"
    "wSXQtaFxycRphopohA56vyT6dP0BMWJQuDvWBUyxYWApP49XuVkZCFy3BFbJ8ZniuhzkgrRhTN"
    "-L8jUiopUd6mtdPPc7ZLoWcKXrtsTjlNSOH7r2VmJTCeWgVsDCBRceFdLyNy1tfz5ibaTgP-"
    "oL7tUQ1VnRkRoedLm17LoPKtu4-dbGchIaUnYfP1__gbHW48_oj_"
    "QSbutSQYsxtx2ESZ78JFd1kFX0Qs5YL7u2k93Za49S3SuWH9CV7bca0leIzEw";

// kTokenIssuer2 is the same as kToken except that "iss" is
// "https://issuer2.com".
const char kTokenIssuer2[] =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImIzMzE5YTE0NzUxNGRmN2VlNWU0Ym"
    "NkZWU1MTM1MGNjODkwY2M4OWUifQ."
    "eyJpc3MiOiJodHRwczovL2lzc3VlcjIuY29tIiwic3ViIjoiZW5kLXVzZXItaWQiLCJhdWQiOi"
    "JlbmRwb2ludHMtdGVzdC5jbG91ZGVuZHBvaW50c2FwaXMuY29tIiwiaWF0IjoxNDYxNzc5MzIx"
    "LCJleHAiOjI0NjE3ODI5MjF9.iFPb5TSZXGBxzFKJ5FOZDr1sH_-5KEBRvJy5vtanbjX2H-"
    "VU0aloWUeKCUGbtd9HdnHP4I7n-nivI1wltYK-E19aG3xswDZr3I5kg3JyNp-"
    "T9a4EuQ7cue7ofxJi57l7vRDGQ-"
    "9548QoenJP9vkJc4nb70xPF0CriwujaBr91jOaJmvc4W1ivXoIc1QgG9wHdRg8AgeIAaQhvnyj"
    "F_Y9ut23lhAL8miYEs2ggwUSrImQzjed0t0205nz_"
    "3rFZuei5DNEGbDoj6ja5jLvf11W2bCTkitEjYXUbomKm-RegWX6MjT-"
    "zdhuVgY0vUaLFI1OFlKT7ArQQ5HZpw9lt4sKzg";

// kTokenExpired is the same as kToken except that "exp" is "1461782921".
const char kTokenExpired[] =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImIzMzE5YTE0NzUxNGRmN2VlNWU0Ym"
    "NkZWU1MTM1MGNjODkwY2M4OWUifQ."
    "eyJpc3MiOiJodHRwczovL2lzc3VlcjEuY29tIiwic3ViIjoiZW5kLXVzZXItaWQiLCJhdWQiOi"
    "JlbmRwb2ludHMtdGVzdC5jbG91ZGVuZHBvaW50c2FwaXMuY29tIiwiaWF0IjoxNDYxNzc5MzIx"
    "LCJleHAiOjE0NjE3ODI5MjF9.Zlk_QD3FM5u-E7r2Vo4xrPfNUNtewfTmJPZUL_"
    "MaLkWK3Do6ov0B8mj5nzf8fFyw3YTxhN7VjabDfwr2pTfVl0PA8Kx8XLz3R_"
    "apAbP0UhKEBgYhv5GCLhiTTuq01QCDnhvcTVXRorWfe6ocC2GPTIHVA_M5U1v5nPopP-"
    "Kp68fMQ3sro7mNIs-NtWQsjPEyk1cHu1wH0_pUjcVjex8SQb6vRxAEsF4sZq17cCRI-"
    "6miwxYNzYyVCv-csNsxgTH8_kZbfXgk9WbJKE4k7XhIsU4K_cLmz8OysRw9IpsWQpmhHGHMXu-"
    "-QBSDC3_5Lp5RU5oaEjHRd9AvXVsz1K1iaQ";

// kTokenBadAud is the same as kToken except that "aud" is "some-audience".
const char kTokenBadAud[] =
    "eyJhbGciOiJSUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImIzMzE5YTE0NzUxNGRmN2VlNWU0Ym"
    "NkZWU1MTM1MGNjODkwY2M4OWUifQ."
    "eyJpc3MiOiJodHRwczovL2lzc3VlcjEuY29tIiwic3ViIjoiZW5kLXVzZXItaWQiLCJhdWQiOi"
    "Jzb21lLWF1ZGllbmNlIiwiaWF0IjoxNDYxNzc5MzIxLCJleHAiOjI0NjE3ODI5MjF9."
    "KFNI2R5r9IdjvAWFEvrM6q7dHNkrbLfPZNK7u8XQmn4vbMChHVW0gYYbO-"
    "eaLPl0xnGX9q40xiMkM3blZ4oRPTj5YF_GcG9S-2fTPXdbmOtbHbpfMG1W26n2ESH2UKpEAL-"
    "wRWbC7ea2dsdHE2zEbQfbLAwRjX3m3JiJlLHFJkQcrxsj8PwEScNBRJLRrIA1c4EPwQRsiNR7w"
    "VooZYt_8v2ClgdKx8I-iDa4zhOheIVgvOduKARW5p2yyptM__9Pr544ox-R1IO2YYh-"
    "70mLN05YowDM268OOjkdR1wC5vtsXGns5ZmT-h1vdQXluMuz-S2ppR3EqTUip4rBMOSAEQ";

const char kOpenIdContent[] = "{\"jwks_uri\": \"https://issuer1.com/pubkey\"}";

const char kPubkey[] =
    "{\"keys\": [{\"kty\": \"RSA\",\"alg\": \"RS256\",\"use\": "
    "\"sig\",\"kid\": \"62a93512c9ee4c7f8067b5a216dade2763d32a47\",\"n\": "
    "\"0YWnm_eplO9BFtXszMRQNL5UtZ8HJdTH2jK7vjs4XdLkPW7YBkkm_"
    "2xNgcaVpkW0VT2l4mU3KftR-6s3Oa5Rnz5BrWEUkCTVVolR7VYksfqIB2I_"
    "x5yZHdOiomMTcm3DheUUCgbJRv5OKRnNqszA4xHn3tA3Ry8VO3X7BgKZYAUh9fyZTFLlkeAh0-"
    "bLK5zvqCmKW5QgDIXSxUTJxPjZCgfx1vmAfGqaJb-"
    "nvmrORXQ6L284c73DUL7mnt6wj3H6tVqPKA27j56N0TB1Hfx4ja6Slr8S4EB3F1luYhATa1PKU"
    "SH8mYDW11HolzZmTQpRoLV8ZoHbHEaTfqX_aYahIw\",\"e\": \"AQAB\"},{\"kty\": "
    "\"RSA\",\"alg\": \"RS256\",\"use\": \"sig\",\"kid\": "
    "\"b3319a147514df7ee5e4bcdee51350cc890cc89e\",\"n\": "
    "\"qDi7Tx4DhNvPQsl1ofxxc2ePQFcs-L0mXYo6TGS64CY_"
    "2WmOtvYlcLNZjhuddZVV2X88m0MfwaSA16wE-"
    "RiKM9hqo5EY8BPXj57CMiYAyiHuQPp1yayjMgoE1P2jvp4eqF-"
    "BTillGJt5W5RuXti9uqfMtCQdagB8EC3MNRuU_KdeLgBy3lS3oo4LOYd-"
    "74kRBVZbk2wnmmb7IhP9OoLc1-7-9qU1uhpDxmE6JwBau0mDSwMnYDS4G_ML17dC-"
    "ZDtLd1i24STUw39KH0pcSdfFbL2NtEZdNeam1DDdk0iUtJSPZliUHJBI_pj8M-2Mn_"
    "oA8jBuI8YKwBqYkZCN1I95Q\",\"e\": \"AQAB\"}]}";

const char kIssuer1OpenIdUrl[] =
    "https://issuer1.com/.well-known/openid-configuration";

const char kIssuer1PubkeyUrl[] = "https://issuer1.com/pubkey";
const char kIssuer2PubkeyUrl[] = "https://issuer2.com/pubkey";

const char kOpenIdFailUrl[] =
    "http://openid_fail/.well-known/openid-configuration";

class CheckAuthTest : public ::testing::Test {
 public:
  void SetUp() {
    std::unique_ptr<MockApiManagerEnvironment> env(
        new ::testing::NiceMock<MockApiManagerEnvironment>());
    // save the raw pointer of env before calling std::move(env).
    raw_env_ = env.get();

    std::unique_ptr<Config> config =
        Config::Create(raw_env_, kServiceConfig, "");
    ASSERT_NE(config.get(), nullptr);

    service_context_ = std::make_shared<context::ServiceContext>(
        std::move(env), std::move(config));
    ASSERT_NE(service_context_.get(), nullptr);

    std::unique_ptr<MockRequest> request(
        new ::testing::NiceMock<MockRequest>());
    // save the raw pointer of request before calling std::move(request).
    raw_request_ = request.get();

    EXPECT_CALL(*raw_request_, GetRequestHTTPMethod())
        .WillOnce(Return(std::string("GET")));
    EXPECT_CALL(*raw_request_, GetRequestPath())
        .WillOnce(Return(std::string("/ListShelves")));
    EXPECT_CALL(*raw_request_, FindQuery(_, _))
        .WillOnce(Invoke([](const std::string &, std::string *apikey) {
          *apikey = "apikey";
          return true;
        }));
    EXPECT_CALL(*raw_request_, FindHeader("referer", _))
        .WillOnce(Invoke([](const std::string &, std::string *http_referer) {
          *http_referer = "";
          return true;
        }));
    EXPECT_CALL(*raw_request_, FindHeader("X-Cloud-Trace-Context", _))
        .WillOnce(Invoke([](const std::string &, std::string *trace_context) {
          *trace_context = "";
          return true;
        }));
    context_ = std::make_shared<context::RequestContext>(service_context_,
                                                         std::move(request));
    EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_request_));
  }

  MockApiManagerEnvironment *raw_env_;
  std::shared_ptr<context::ServiceContext> service_context_;
  MockRequest *raw_request_;
  std::shared_ptr<context::RequestContext> context_;
};

// Possitive test.
// Step 1: Check auth workflow that involves openID discovery and fetching
//         public key.
// Step 2. Use the same auth token, which should be cached in JWT cache.
// Step 3. User a different auth token signed by the same issuer. This time,
//         token is not cached, but key is cached.
TEST_F(CheckAuthTest, TestOKAuth) {
  // Step 1. Check auth requires open ID discovery and fetching public key.
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kBearer) + std::string(kToken);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kToken)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .Times(2)
      .WillOnce(Invoke([](HTTPRequest *req) -> Status {
        EXPECT_EQ(req->url(), kIssuer1OpenIdUrl);
        std::string body(kOpenIdContent);
        req->OnComplete(Status::OK, std::move(body));
        return Status::OK;
      }))
      .WillOnce(Invoke([](HTTPRequest *req) -> Status {
        EXPECT_EQ(req->url(), kIssuer1PubkeyUrl);
        std::string body(kPubkey);
        req->OnComplete(Status::OK, std::move(body));
        return Status::OK;
      }));
  EXPECT_CALL(*raw_request_, SetUserInfo(AllOf(Field(&UserInfo::id, kSub),
                                               Field(&UserInfo::issuer, kIss))))
      .Times(1);

  CheckAuth(context_, [](Status status) { ASSERT_TRUE(status.ok()); });

  EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_request_));
  EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_env_));

  // Step 2. Check auth with the same auth token. This time the token is cached.
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kBearer) + std::string(kToken);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kToken)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_)).Times(0);
  EXPECT_CALL(*raw_request_, SetUserInfo(AllOf(Field(&UserInfo::id, kSub),
                                               Field(&UserInfo::issuer, kIss))))
      .Times(1);

  CheckAuth(context_, [](Status status) { ASSERT_TRUE(status.ok()); });

  EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_request_));
  EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_env_));

  // Step 3. Check auth with a different token signed by the same issuer.
  // In this case, the token is not in the cache, but key is cached.
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kBearer) + std::string(kToken2);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kToken2)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_)).Times(0);
  EXPECT_CALL(*raw_request_, SetUserInfo(AllOf(Field(&UserInfo::id, kSub2),
                                               Field(&UserInfo::issuer, kIss))))
      .Times(1);

  CheckAuth(context_, [](Status status) { ASSERT_TRUE(status.ok()); });
}

// Negative test: Test the case that openID discovery failed.
// Step 1. Try to fetch key URI via OpenID discovery but failed.
// Step 2. Use a different token signed by the same issuer, no HTTP request
//         is sent this time because the failure result was cached.
TEST_F(CheckAuthTest, TestOpenIdFailed) {
  // Step 1. Try to fetch key URI via OpenID discovery but failed.
  // Use FindQuery to get auth token.
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string();
        return false;
      }));
  EXPECT_CALL(*raw_request_, FindQuery(kAccessTokenName, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kTokenOpenIdFail);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kTokenOpenIdFail)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([](HTTPRequest *req) -> Status {
        EXPECT_EQ(req->url(), kOpenIdFailUrl);
        std::string body("");
        req->OnComplete(Status::OK, std::move(body));
        return Status::OK;
      }));

  CheckAuth(context_, [](Status status) {
    ASSERT_EQ(status.code(), Code::UNAUTHENTICATED);
    ASSERT_EQ(status.message(),
              "JWT validation failed: Unable to fetch "
              "URI of the key via OpenID discovery");
  });

  EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_request_));
  EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_env_));

  // Step 2. Use a different token signed by the same issuer, no HTTP request
  //         is sent this time because the failure result was cached.
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kBearer) + std::string(kTokenOpenIdFail2);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kTokenOpenIdFail2)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_)).Times(0);

  CheckAuth(context_, [](Status status) {
    ASSERT_EQ(status.code(), Code::UNAUTHENTICATED);
    ASSERT_EQ(status.message(),
              "JWT validation failed: "
              "Cannot determine the URI of the key");
  });
}

// jwks_uri is already specified in service config. Hence, no need to
// do openID discovery.
TEST_F(CheckAuthTest, TestNoOpenId) {
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kBearer) + std::string(kTokenIssuer2);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kTokenIssuer2)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_))
      .WillOnce(Invoke([](HTTPRequest *req) -> Status {
        EXPECT_EQ(req->url(), kIssuer2PubkeyUrl);
        std::string body(kPubkey);
        req->OnComplete(Status::OK, std::move(body));
        return Status::OK;
      }));
  EXPECT_CALL(*raw_request_,
              SetUserInfo(AllOf(Field(&UserInfo::id, kSub),
                                Field(&UserInfo::issuer, kIss2))))
      .Times(1);

  CheckAuth(context_, [](Status status) { ASSERT_TRUE(status.ok()); });
}

// Negative test: invalid token and expired token.
TEST_F(CheckAuthTest, TestInvalidToken) {
  // Invalid token.
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = "bad_token";
        return true;
      }));

  CheckAuth(context_, [](Status status) {
    ASSERT_EQ(status.code(), Code::UNAUTHENTICATED);
    ASSERT_EQ(status.message(),
              "JWT validation failed: "
              "Missing or invalid credentials");
  });

  EXPECT_TRUE(Mock::VerifyAndClearExpectations(raw_request_));

  // Expired token.
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kBearer) + std::string(kTokenExpired);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kTokenExpired)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_)).Times(0);

  CheckAuth(context_, [](Status status) {
    ASSERT_EQ(status.code(), Code::UNAUTHENTICATED);
    ASSERT_EQ(status.message(),
              "JWT validation failed: TIME_CONSTRAINT_FAILURE");
  });
}

// Negative test: bad audience
TEST_F(CheckAuthTest, TestBadAudience) {
  EXPECT_CALL(*raw_request_, FindHeader(kAuthHeader, _))
      .WillOnce(Invoke([](const std::string &, std::string *token) {
        *token = std::string(kBearer) + std::string(kTokenBadAud);
        return true;
      }));
  EXPECT_CALL(*raw_request_, SetAuthToken(kTokenBadAud)).Times(1);
  EXPECT_CALL(*raw_env_, DoRunHTTPRequest(_)).Times(0);

  CheckAuth(context_, [](Status status) {
    ASSERT_EQ(status.code(), Code::UNAUTHENTICATED);
    ASSERT_EQ(status.message(), "JWT validation failed: Audience not allowed");
  });
}

}  // namespace

}  // namespace api_manager
}  // namespace google