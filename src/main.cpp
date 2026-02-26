/**
 * C++ Transcription Starter - Backend Server
 *
 * This is a simple HTTP server that provides a transcription API endpoint
 * powered by Deepgram's Speech-to-Text service. It's designed to be easily
 * modified and extended for your own projects.
 *
 * Key Features:
 * - Single API endpoint: POST /api/transcription
 * - Accepts file uploads (multipart/form-data)
 * - CORS enabled for frontend communication
 * - JWT session auth (HS256 via OpenSSL HMAC)
 * - Pure API server (frontend served separately)
 */

#include <crow.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

using json = nlohmann::json;

// ============================================================================
// SECTION 1: CONFIGURATION - Customize these values for your needs
// ============================================================================

/**
 * Default transcription model to use when none is specified.
 * Options: "nova-3", "nova-2", "nova", "enhanced", "base"
 * See: https://developers.deepgram.com/docs/models-languages-overview
 */
static const std::string DEFAULT_MODEL = "nova-3";

/**
 * Server configuration, overridable via environment variables.
 */
struct Config {
    std::string port;
    std::string host;
};

/**
 * Reads PORT and HOST from the environment with sensible defaults.
 */
Config loadConfig() {
    Config cfg;
    const char* port = std::getenv("PORT");
    cfg.port = port ? port : "8081";
    const char* host = std::getenv("HOST");
    cfg.host = host ? host : "0.0.0.0";
    return cfg;
}

// ============================================================================
// SECTION 2: SESSION AUTH - JWT tokens for production security
// ============================================================================

/** Session secret used to sign JWTs. Auto-generated if not set via env. */
static std::string sessionSecret;

/** JWT expiry duration in seconds (1 hour). */
static const int JWT_EXPIRY_SECONDS = 3600;

/**
 * Base64url-encode raw bytes (no padding).
 */
static std::string base64urlEncode(const unsigned char* data, size_t len) {
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(4 * ((len + 2) / 3));
    for (size_t i = 0; i < len; i += 3) {
        unsigned int n = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<unsigned int>(data[i + 2]);
        result.push_back(table[(n >> 18) & 0x3F]);
        result.push_back(table[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? table[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? table[n & 0x3F] : '=');
    }
    // Convert to base64url: replace + with -, / with _, strip padding
    for (auto& c : result) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    result.erase(std::remove(result.begin(), result.end(), '='), result.end());
    return result;
}

/**
 * Base64url-encode a string.
 */
static std::string base64urlEncode(const std::string& input) {
    return base64urlEncode(
        reinterpret_cast<const unsigned char*>(input.data()), input.size());
}

/**
 * Base64url-decode a string to raw bytes.
 */
static std::string base64urlDecode(const std::string& input) {
    std::string padded = input;
    // Convert base64url back to base64
    for (auto& c : padded) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    // Add padding
    while (padded.size() % 4 != 0) padded.push_back('=');

    static const int decodeTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };

    std::string result;
    result.reserve(padded.size() * 3 / 4);
    for (size_t i = 0; i < padded.size(); i += 4) {
        int a = decodeTable[static_cast<unsigned char>(padded[i])];
        int b = decodeTable[static_cast<unsigned char>(padded[i + 1])];
        int c = decodeTable[static_cast<unsigned char>(padded[i + 2])];
        int d = decodeTable[static_cast<unsigned char>(padded[i + 3])];
        if (a < 0 || b < 0) break;
        result.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (c >= 0) result.push_back(static_cast<char>(((b & 0x0F) << 4) | (c >> 2)));
        if (d >= 0) result.push_back(static_cast<char>(((c & 0x03) << 6) | d));
    }
    return result;
}

/**
 * Sign data with HMAC-SHA256 using the session secret.
 */
static std::string hmacSHA256(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int resultLen = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()),
         data.size(),
         result, &resultLen);
    return base64urlEncode(result, resultLen);
}

/**
 * Generate a JWT token with HS256 signing.
 */
static std::string generateJWT() {
    auto now = std::time(nullptr);
    auto exp = now + JWT_EXPIRY_SECONDS;

    json header = {{"alg", "HS256"}, {"typ", "JWT"}};
    json payload = {{"iat", now}, {"exp", exp}};

    std::string headerEnc = base64urlEncode(header.dump());
    std::string payloadEnc = base64urlEncode(payload.dump());
    std::string signingInput = headerEnc + "." + payloadEnc;
    std::string signature = hmacSHA256(sessionSecret, signingInput);

    return signingInput + "." + signature;
}

/**
 * Validate a JWT token. Returns true if valid and not expired.
 */
static bool validateJWT(const std::string& token, std::string& errorMsg) {
    // Split token into 3 parts
    size_t dot1 = token.find('.');
    if (dot1 == std::string::npos) {
        errorMsg = "Invalid session token";
        return false;
    }
    size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) {
        errorMsg = "Invalid session token";
        return false;
    }

    std::string headerEnc = token.substr(0, dot1);
    std::string payloadEnc = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string signatureEnc = token.substr(dot2 + 1);

    // Verify signature
    std::string signingInput = headerEnc + "." + payloadEnc;
    std::string expectedSig = hmacSHA256(sessionSecret, signingInput);
    if (signatureEnc != expectedSig) {
        errorMsg = "Invalid session token";
        return false;
    }

    // Verify header alg
    try {
        std::string headerJson = base64urlDecode(headerEnc);
        json header = json::parse(headerJson);
        if (header.value("alg", "") != "HS256") {
            errorMsg = "Invalid session token";
            return false;
        }
    } catch (...) {
        errorMsg = "Invalid session token";
        return false;
    }

    // Verify expiration
    try {
        std::string payloadJson = base64urlDecode(payloadEnc);
        json payload = json::parse(payloadJson);
        auto now = std::time(nullptr);
        if (payload.contains("exp") && payload["exp"].get<int64_t>() < now) {
            errorMsg = "Session expired, please refresh the page";
            return false;
        }
    } catch (...) {
        errorMsg = "Invalid session token";
        return false;
    }

    return true;
}

/**
 * Initialize session secret from env or generate one.
 */
static void initSessionSecret() {
    const char* secret = std::getenv("SESSION_SECRET");
    if (secret && secret[0] != '\0') {
        sessionSecret = secret;
    } else {
        unsigned char buf[32];
        RAND_bytes(buf, sizeof(buf));
        std::ostringstream oss;
        for (int i = 0; i < 32; ++i) {
            oss << std::hex << std::setfill('0') << std::setw(2)
                << static_cast<int>(buf[i]);
        }
        sessionSecret = oss.str();
    }
}

// ============================================================================
// SECTION 3: API KEY LOADING - Load Deepgram API key from .env
// ============================================================================

/** Deepgram API key loaded at startup. */
static std::string apiKey;

/**
 * Read the Deepgram API key from the environment.
 * Exits with a helpful error message if not found.
 */
static std::string loadAPIKey() {
    const char* key = std::getenv("DEEPGRAM_API_KEY");
    if (key && key[0] != '\0') {
        return key;
    }
    std::cerr << "\n  ERROR: Deepgram API key not found!\n" << std::endl;
    std::cerr << "Please set your API key using one of these methods:\n" << std::endl;
    std::cerr << "1. Create a .env file (recommended):" << std::endl;
    std::cerr << "   DEEPGRAM_API_KEY=your_api_key_here\n" << std::endl;
    std::cerr << "2. Environment variable:" << std::endl;
    std::cerr << "   export DEEPGRAM_API_KEY=your_api_key_here\n" << std::endl;
    std::cerr << "Get your API key at: https://console.deepgram.com\n" << std::endl;
    std::exit(1);
}

// ============================================================================
// SECTION 4: SETUP - Initialize configuration and .env loading
// ============================================================================

/**
 * Load .env file by reading KEY=VALUE lines and setting them as env vars.
 * Ignores comments and blank lines. Silently skips if file not found.
 */
static void loadDotEnv(const std::string& path = ".env") {
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        // Trim leading whitespace
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        // Skip comments and empty lines
        if (line.empty() || line[0] == '#') continue;

        // Find KEY=VALUE separator
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);

        // Trim trailing whitespace from key
        size_t keyEnd = key.find_last_not_of(" \t");
        if (keyEnd != std::string::npos) key = key.substr(0, keyEnd + 1);

        // Trim surrounding whitespace and quotes from value
        size_t valStart = value.find_first_not_of(" \t");
        if (valStart != std::string::npos) value = value.substr(valStart);
        size_t valEnd = value.find_last_not_of(" \t\r\n");
        if (valEnd != std::string::npos) value = value.substr(0, valEnd + 1);

        // Strip surrounding quotes (single or double)
        if (value.size() >= 2 &&
            ((value.front() == '"' && value.back() == '"') ||
             (value.front() == '\'' && value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        // Only set if not already in environment (env vars take precedence)
        if (!std::getenv(key.c_str())) {
            setenv(key.c_str(), value.c_str(), 0);
        }
    }
}

// ============================================================================
// SECTION 5: HELPER FUNCTIONS - JSON response and CORS utilities
// ============================================================================

/**
 * Build a structured error envelope suitable for the frontend to display.
 */
static json formatErrorResponse(const std::string& errMsg, int statusCode,
                                const std::string& code = "") {
    std::string errType = "TranscriptionError";
    if (statusCode == 400) errType = "ValidationError";

    std::string errCode = code;
    if (errCode.empty()) {
        errCode = (statusCode == 400) ? "MISSING_INPUT" : "TRANSCRIPTION_FAILED";
    }

    return {
        {"error", {
            {"type", errType},
            {"code", errCode},
            {"message", errMsg},
            {"details", {
                {"originalError", errMsg}
            }}
        }}
    };
}

/**
 * Add CORS headers to a Crow response.
 */
static void addCorsHeaders(crow::response& res) {
    res.add_header("Access-Control-Allow-Origin", "*");
    res.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.add_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

/**
 * Return the first non-empty string from a list.
 */
static std::string firstNonEmpty(std::initializer_list<std::string> vals) {
    for (const auto& v : vals) {
        if (!v.empty()) return v;
    }
    return "";
}

// ============================================================================
// SECTION 6: DEEPGRAM API CLIENT - Direct HTTP calls to Deepgram REST API
// ============================================================================

/** Callback for libcurl to write response data into a string. */
static size_t curlWriteCallback(void* contents, size_t size, size_t nmemb,
                                void* userp) {
    size_t totalSize = size * nmemb;
    auto* response = static_cast<std::string*>(userp);
    response->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

/**
 * Send audio bytes to the Deepgram /v1/listen endpoint and return the
 * parsed JSON response. Query parameters control model selection and
 * feature flags.
 */
static std::pair<json, std::string> callDeepgramTranscription(
    const std::string& audioData,
    const std::unordered_map<std::string, std::string>& params) {

    // Build URL with query parameters
    std::string url = "https://api.deepgram.com/v1/listen";
    bool first = true;
    for (const auto& [key, value] : params) {
        if (!value.empty()) {
            url += (first ? "?" : "&");
            first = false;

            // Simple URL encoding for parameter values
            char* escapedKey = curl_easy_escape(nullptr, key.c_str(),
                                                static_cast<int>(key.size()));
            char* escapedVal = curl_easy_escape(nullptr, value.c_str(),
                                                static_cast<int>(value.size()));
            url += std::string(escapedKey) + "=" + std::string(escapedVal);
            curl_free(escapedKey);
            curl_free(escapedVal);
        }
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        return {json(), "Failed to initialize HTTP client"};
    }

    std::string responseBody;
    struct curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Token " + apiKey;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/octet-stream");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, audioData.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(audioData.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::string error = "Deepgram API request failed: " +
                            std::string(curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return {json(), error};
    }

    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (httpCode != 200) {
        return {json(), "Deepgram API returned status " +
                            std::to_string(httpCode) + ": " + responseBody};
    }

    try {
        json result = json::parse(responseBody);
        return {result, ""};
    } catch (const json::parse_error& e) {
        return {json(), "Failed to parse Deepgram response: " +
                            std::string(e.what())};
    }
}

// ============================================================================
// SECTION 7: RESPONSE FORMATTING - Shape Deepgram responses for the frontend
// ============================================================================

/**
 * Extract relevant fields from the raw Deepgram API response and return
 * a simplified structure the frontend expects.
 */
static std::pair<json, std::string> formatTranscriptionResponse(
    const json& dgResponse, const std::string& modelName) {

    // Navigate: results -> channels[0] -> alternatives[0]
    if (!dgResponse.contains("results") ||
        !dgResponse["results"].contains("channels") ||
        !dgResponse["results"]["channels"].is_array() ||
        dgResponse["results"]["channels"].empty()) {
        return {json(), "No transcription results returned from Deepgram"};
    }

    const auto& channel = dgResponse["results"]["channels"][0];
    if (!channel.contains("alternatives") ||
        !channel["alternatives"].is_array() ||
        channel["alternatives"].empty()) {
        return {json(), "No transcription results returned from Deepgram"};
    }

    const auto& alt = channel["alternatives"][0];

    // Build metadata from top-level metadata field
    json metadata = {{"model_name", modelName}};
    if (dgResponse.contains("metadata")) {
        const auto& meta = dgResponse["metadata"];
        if (meta.contains("model_uuid"))
            metadata["model_uuid"] = meta["model_uuid"];
        if (meta.contains("request_id"))
            metadata["request_id"] = meta["request_id"];
    }

    json response = {
        {"transcript", alt.value("transcript", "")},
        {"words", alt.value("words", json::array())},
        {"metadata", metadata}
    };

    // Add optional duration if present
    if (dgResponse.contains("metadata") &&
        dgResponse["metadata"].contains("duration")) {
        response["duration"] = dgResponse["metadata"]["duration"];
    }

    return {response, ""};
}

// ============================================================================
// SECTION 8: SESSION ROUTES - Auth endpoints (unprotected)
// ============================================================================

/**
 * Validate Bearer token from Authorization header.
 * Returns true if valid; sets errorJson on failure.
 */
static bool validateBearerToken(const crow::request& req,
                                crow::response& res) {
    std::string authHeader = req.get_header_value("Authorization");
    if (authHeader.empty() ||
        authHeader.substr(0, 7) != "Bearer ") {
        json errBody = {
            {"error", {
                {"type", "AuthenticationError"},
                {"code", "MISSING_TOKEN"},
                {"message",
                 "Authorization header with Bearer token is required"}
            }}
        };
        res.code = 401;
        res.set_header("Content-Type", "application/json");
        addCorsHeaders(res);
        res.write(errBody.dump());
        res.end();
        return false;
    }

    std::string token = authHeader.substr(7);
    std::string errorMsg;
    if (!validateJWT(token, errorMsg)) {
        json errBody = {
            {"error", {
                {"type", "AuthenticationError"},
                {"code", "INVALID_TOKEN"},
                {"message", errorMsg}
            }}
        };
        res.code = 401;
        res.set_header("Content-Type", "application/json");
        addCorsHeaders(res);
        res.write(errBody.dump());
        res.end();
        return false;
    }

    return true;
}

// ============================================================================
// SECTION 9: API ROUTES - Define your API endpoints here
// ============================================================================

// Routes are registered in main() below using Crow's lambda-based API.

// ============================================================================
// SECTION 10: SERVER START
// ============================================================================

int main() {
    // Load .env file (ignore error if not present)
    loadDotEnv();

    // Initialize components
    Config cfg = loadConfig();
    initSessionSecret();
    apiKey = loadAPIKey();

    // Initialize libcurl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    crow::SimpleApp app;

    // --- CORS preflight handler for all routes ---
    CROW_ROUTE(app, "/api/<path>")
        .methods(crow::HTTPMethod::OPTIONS)
        ([](const crow::request&, const std::string&) {
            crow::response res(204);
            addCorsHeaders(res);
            return res;
        });

    CROW_ROUTE(app, "/health")
        .methods(crow::HTTPMethod::OPTIONS)
        ([]() {
            crow::response res(204);
            addCorsHeaders(res);
            return res;
        });

    // --- GET /api/session - Issue JWT (unprotected) ---
    CROW_ROUTE(app, "/api/session")
        .methods(crow::HTTPMethod::GET)
        ([](const crow::request&) {
            std::string token = generateJWT();
            json body = {{"token", token}};
            crow::response res(200);
            res.set_header("Content-Type", "application/json");
            addCorsHeaders(res);
            res.write(body.dump());
            return res;
        });

    // --- POST /api/transcription - File upload transcription (protected) ---
    CROW_ROUTE(app, "/api/transcription")
        .methods(crow::HTTPMethod::POST)
        ([](const crow::request& req) {
            crow::response res;

            // Validate JWT
            if (!validateBearerToken(req, res)) {
                return res;
            }

            // Parse multipart form data
            crow::multipart::message msg(req);
            std::string audioData;

            // Find the "file" part
            for (const auto& part : msg.parts) {
                auto it = part.headers.find("Content-Disposition");
                if (it != part.headers.end()) {
                    // Check if the params map contains a "name" key with "file"
                    auto nameIt = it->second.params.find("name");
                    if (nameIt != it->second.params.end() &&
                        nameIt->second == "file") {
                        audioData = part.body;
                        break;
                    }
                }
            }

            if (audioData.empty()) {
                json errBody = formatErrorResponse(
                    "Either file or url must be provided", 400, "MISSING_INPUT");
                res.code = 400;
                res.set_header("Content-Type", "application/json");
                addCorsHeaders(res);
                res.write(errBody.dump());
                return res;
            }

            // Extract query parameters and form values
            auto urlParams = crow::query_string("?" + req.url_params.get_query_string_raw());

            // Model parameter
            std::string model;
            const char* modelParam = urlParams.get("model");
            if (modelParam) model = modelParam;

            // Also check form fields for model
            if (model.empty()) {
                for (const auto& part : msg.parts) {
                    auto it = part.headers.find("Content-Disposition");
                    if (it != part.headers.end()) {
                        auto nameIt = it->second.params.find("name");
                        if (nameIt != it->second.params.end() &&
                            nameIt->second == "model" && !part.body.empty()) {
                            model = part.body;
                            break;
                        }
                    }
                }
            }
            if (model.empty()) model = DEFAULT_MODEL;

            // Build Deepgram query parameters
            auto getFormValue = [&](const std::string& name) -> std::string {
                for (const auto& part : msg.parts) {
                    auto it = part.headers.find("Content-Disposition");
                    if (it != part.headers.end()) {
                        auto nameIt = it->second.params.find("name");
                        if (nameIt != it->second.params.end() &&
                            nameIt->second == name && !part.body.empty()) {
                            return part.body;
                        }
                    }
                }
                return "";
            };

            auto getParam = [&](const std::string& name) -> std::string {
                const char* val = urlParams.get(name.c_str());
                if (val) return val;
                return getFormValue(name);
            };

            std::unordered_map<std::string, std::string> params;
            params["model"] = model;
            params["language"] = firstNonEmpty({
                getParam("language"), "en"});
            params["smart_format"] = firstNonEmpty({
                getParam("smart_format"), "true"});

            // Optional boolean feature flags
            for (const auto& key : {"diarize", "punctuate", "paragraphs",
                                    "utterances", "filler_words"}) {
                std::string val = getParam(key);
                if (!val.empty()) {
                    params[key] = val;
                }
            }

            // Call Deepgram REST API
            auto [dgResponse, dgError] =
                callDeepgramTranscription(audioData, params);
            if (!dgError.empty()) {
                CROW_LOG_ERROR << "Transcription error: " << dgError;
                json errBody = formatErrorResponse(
                    "An error occurred during transcription", 500,
                    "TRANSCRIPTION_FAILED");
                res.code = 500;
                res.set_header("Content-Type", "application/json");
                addCorsHeaders(res);
                res.write(errBody.dump());
                return res;
            }

            // Format and return response
            auto [formatted, fmtError] =
                formatTranscriptionResponse(dgResponse, model);
            if (!fmtError.empty()) {
                CROW_LOG_ERROR << "Response formatting error: " << fmtError;
                json errBody = formatErrorResponse(
                    fmtError, 500, "TRANSCRIPTION_FAILED");
                res.code = 500;
                res.set_header("Content-Type", "application/json");
                addCorsHeaders(res);
                res.write(errBody.dump());
                return res;
            }

            res.code = 200;
            res.set_header("Content-Type", "application/json");
            addCorsHeaders(res);
            res.write(formatted.dump());
            return res;
        });

    // --- GET /api/metadata - Return [meta] from deepgram.toml (unprotected) ---
    CROW_ROUTE(app, "/api/metadata")
        .methods(crow::HTTPMethod::GET)
        ([](const crow::request&) {
            crow::response res;
            try {
                auto config = toml::parse_file("deepgram.toml");
                auto meta = config["meta"];
                if (!meta) {
                    json errBody = {
                        {"error", "INTERNAL_SERVER_ERROR"},
                        {"message", "Missing [meta] section in deepgram.toml"}
                    };
                    res.code = 500;
                    res.set_header("Content-Type", "application/json");
                    addCorsHeaders(res);
                    res.write(errBody.dump());
                    return res;
                }

                // Convert TOML table to JSON
                json metaJson = json::object();
                auto* tbl = meta.as_table();
                if (tbl) {
                    for (auto&& [k, v] : *tbl) {
                        std::string key(k.str());
                        if (v.is_string())
                            metaJson[key] = v.as_string()->get();
                        else if (v.is_integer())
                            metaJson[key] = v.as_integer()->get();
                        else if (v.is_boolean())
                            metaJson[key] = v.as_boolean()->get();
                        else if (v.is_floating_point())
                            metaJson[key] = v.as_floating_point()->get();
                        else if (v.is_array()) {
                            json arr = json::array();
                            for (auto&& elem : *v.as_array()) {
                                if (elem.is_string())
                                    arr.push_back(elem.as_string()->get());
                                else if (elem.is_integer())
                                    arr.push_back(elem.as_integer()->get());
                                else if (elem.is_boolean())
                                    arr.push_back(elem.as_boolean()->get());
                            }
                            metaJson[key] = arr;
                        }
                    }
                }

                res.code = 200;
                res.set_header("Content-Type", "application/json");
                addCorsHeaders(res);
                res.write(metaJson.dump());
            } catch (const toml::parse_error& err) {
                CROW_LOG_ERROR << "Error reading deepgram.toml: " << err.what();
                json errBody = {
                    {"error", "INTERNAL_SERVER_ERROR"},
                    {"message", "Failed to read metadata from deepgram.toml"}
                };
                res.code = 500;
                res.set_header("Content-Type", "application/json");
                addCorsHeaders(res);
                res.write(errBody.dump());
            }
            return res;
        });

    // --- GET /health - Health check ---
    CROW_ROUTE(app, "/health")
        .methods(crow::HTTPMethod::GET)
        ([]() {
            json body = {{"status", "ok"}};
            crow::response res(200);
            res.set_header("Content-Type", "application/json");
            addCorsHeaders(res);
            res.write(body.dump());
            return res;
        });

    // Start server
    int port = std::stoi(cfg.port);
    std::string separator(70, '=');
    std::cout << "\n" << separator << std::endl;
    std::cout << "  Backend API running at http://localhost:" << cfg.port
              << std::endl;
    std::cout << "  GET  /api/session" << std::endl;
    std::cout << "  POST /api/transcription (auth required)" << std::endl;
    std::cout << "  GET  /api/metadata" << std::endl;
    std::cout << "  GET  /health" << std::endl;
    std::cout << separator << "\n" << std::endl;

    app.bindaddr(cfg.host).port(port).multithreaded().run();

    curl_global_cleanup();
    return 0;
}
